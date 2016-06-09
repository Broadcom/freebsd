/*-
 * Broadcom NetXtreme-C/E network driver.
 *
 * Copyright (c) 2016 Broadcom, All Rights Reserved.
 * The term Broadcom refers to Broadcom Limited and/or its subsidiaries
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS'
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/endian.h>

#include "bnxt.h"
#include "bnxt_hwrm.h"
#include "hsi_struct_def.h"
//#include "decode_hsi.h"

static int bnxt_hwrm_err_map(uint16_t err);
static inline int _is_valid_ether_addr(uint8_t *);
static inline void get_random_ether_addr(uint8_t *);
static void	bnxt_hwrm_set_link_common(struct bnxt_softc *softc,
	 	    struct hwrm_port_phy_cfg_input *req);
static void	bnxt_hwrm_set_pause_common(struct bnxt_softc *softc,
		    struct hwrm_port_phy_cfg_input *req);
static void	bnxt_hwrm_set_eee(struct bnxt_softc *softc,
		    struct hwrm_port_phy_cfg_input *req);

static int
bnxt_hwrm_err_map(uint16_t err)
{
	int rc;

	switch (err) {
	case HWRM_ERR_CODE_SUCCESS:
		return 0;
	case HWRM_ERR_CODE_INVALID_PARAMS:
	case HWRM_ERR_CODE_INVALID_FLAGS:
	case HWRM_ERR_CODE_INVALID_ENABLES:
		return EINVAL;
	case HWRM_ERR_CODE_RESOURCE_ACCESS_DENIED:
		return EACCES;
	case HWRM_ERR_CODE_RESOURCE_ALLOC_ERROR:
		return ENOMEM;
	case HWRM_ERR_CODE_CMD_NOT_SUPPORTED:
		return ENOSYS;
	case HWRM_ERR_CODE_FAIL:
	case HWRM_ERR_CODE_HWRM_ERROR:
	case HWRM_ERR_CODE_UNKNOWN_ERR:
	default:
		return EDOOFUS;
	}

	return rc;
}

int
bnxt_alloc_hwrm_dma_mem(struct bnxt_softc *softc)
{
	int rc;

	rc = iflib_dma_alloc(softc->ctx, PAGE_SIZE, &softc->hwrm_cmd_resp, BUS_DMA_NOWAIT);
	return rc;
}

void
bnxt_free_hwrm_dma_mem(struct bnxt_softc *softc)
{
	if (softc->hwrm_cmd_resp.idi_vaddr)
		iflib_dma_free(&softc->hwrm_cmd_resp);
	softc->hwrm_cmd_resp.idi_vaddr = NULL;
	return;
}

void
bnxt_hwrm_cmd_hdr_init(struct bnxt_softc *softc, void *request,
    uint16_t req_type, uint16_t cmpl_ring, uint16_t target_id)
{
	struct input *req = request;

	req->req_type = htole16(req_type);
	req->cmpl_ring = htole16(cmpl_ring);
	req->target_id = htole16(target_id);
	req->resp_addr = htole64(softc->hwrm_cmd_resp.idi_paddr);
}

int
_hwrm_send_message(struct bnxt_softc *softc, void *msg, uint32_t msg_len)
{
	struct input *req = msg;
	struct hwrm_err_output *resp = (void *)softc->hwrm_cmd_resp.idi_vaddr;
	uint32_t *data = msg;
	int i;
	uint16_t cp_ring_id;
	uint8_t *valid;
	uint16_t err;

	req->seq_id = htole16(softc->hwrm_cmd_seq++);
	memset(resp, 0, PAGE_SIZE);
	cp_ring_id = le16toh(req->cmpl_ring);

	//decode_hwrm_req(req);

	/* Write request msg to hwrm channel */
	for (i = 0; i < msg_len; i += 4) {
		bus_space_write_4(softc->bar[0].tag,
				  softc->bar[0].handle,
				  i, *data);
		data++;
	}

	/* Clear to the end of the request buffer */
	for (i = msg_len; i < HWRM_MAX_REQ_LEN; i += 4)
		bus_space_write_4(softc->bar[0].tag, softc->bar[0].handle, i,
		    0);

	/* Ring channel doorbell */
	bus_space_write_4(softc->bar[0].tag,
			  softc->bar[0].handle,
			  0x100, 1);

	/* Check if response len is updated */
	for (i = 0; i < softc->hwrm_cmd_timeo; i++) {
		if (resp->resp_len && resp->resp_len <= 4096)
			break;
		DELAY(100);
	}
	if (i >= softc->hwrm_cmd_timeo) {
		device_printf(softc->dev, "Timeout sending hwrm msg: "
		    "(timeout: %d) msg {0x%x 0x%x} len:%d\n", softc->hwrm_cmd_timeo,
		    le16toh(req->req_type), le16toh(req->seq_id),
		    le16toh(resp->resp_len));
			return ETIMEDOUT;
	}
	/* Last byte of resp contains the valid key */
	valid = (uint8_t *)resp + resp->resp_len - 1;
	for (i = 0; i < softc->hwrm_cmd_timeo; i++) {
		if (*valid == HWRM_RESP_VALID_KEY)
			break;
		DELAY(100);
	}
	if (i >= softc->hwrm_cmd_timeo) {
		device_printf(softc->dev, "Timeout sending hwrm msg: "
		    "(timeout: %d) msg {0x%x 0x%x} len:%d v: %d\n",
		    softc->hwrm_cmd_timeo, le16toh(req->req_type),
		    le16toh(req->seq_id), le16toh(resp->resp_len),
		    *valid);
		return ETIMEDOUT;
	}

	err = le16toh(resp->error_code);
	if (err) {
		device_printf(softc->dev, "HWRM command returned error.  cmd:0x%02x err:0x%02x",
		    le16toh(req->req_type), err);
		return bnxt_hwrm_err_map(err);
	}

	//decode_hwrm_resp(resp);
	return 0;
}

int
hwrm_send_message(struct bnxt_softc *softc, void *msg, uint32_t msg_len)
{
	int rc;

	BNXT_HWRM_LOCK(softc);
	rc = _hwrm_send_message(softc, msg, msg_len);
	BNXT_HWRM_UNLOCK(softc);
	return rc;
}

int
bnxt_hwrm_queue_qportcfg(struct bnxt_softc *softc)
{
	struct hwrm_queue_qportcfg_input req = {0};
	struct hwrm_queue_qportcfg_output *resp = (void *)softc->hwrm_cmd_resp.idi_vaddr;

	int	rc = 0;
	uint8_t	*qptr;

	bnxt_hwrm_cmd_hdr_init(softc, &req, HWRM_QUEUE_QPORTCFG, -1, -1);

	BNXT_HWRM_LOCK(softc);
	rc = _hwrm_send_message(softc, &req, sizeof(req));
	if (rc)
		goto qportcfg_exit;

	if (!resp->max_configurable_queues) {
		rc = -EINVAL;
		goto qportcfg_exit;
	}
	softc->max_tc = resp->max_configurable_queues;
	if (softc->max_tc > BNXT_MAX_QUEUE)
		softc->max_tc = BNXT_MAX_QUEUE;

	qptr = &resp->queue_id0;
	for (int i = 0; i < softc->max_tc; i++) {
		softc->q_info[i].id = *qptr++;
		softc->q_info[i].profile = *qptr++;
	}

qportcfg_exit:
	BNXT_HWRM_UNLOCK(softc);
	return (rc);
}


int
bnxt_hwrm_ver_get(struct bnxt_softc *softc)
{
	struct hwrm_ver_get_input	req = {0};
	struct hwrm_ver_get_output	*resp = (void *)softc->hwrm_cmd_resp.idi_vaddr;
	int				rc;

	softc->hwrm_max_req_len = HWRM_MAX_REQ_LEN;
	softc->hwrm_cmd_timeo = 1000;
	bnxt_hwrm_cmd_hdr_init(softc, &req, HWRM_VER_GET, -1, -1);

	req.hwrm_intf_maj = HWRM_VERSION_MAJOR;
	req.hwrm_intf_min = HWRM_VERSION_MINOR;
	req.hwrm_intf_upd = HWRM_VERSION_UPDATE;

	BNXT_HWRM_LOCK(softc);
	rc = _hwrm_send_message(softc, &req, sizeof(req));
	if (rc)
		goto fail;

	if (resp->hwrm_intf_maj < 1) {
		device_printf(softc->dev, "HWRM interface %d.%d.%d is older than 1.0.0.\n",
		    resp->hwrm_intf_maj, resp->hwrm_intf_min, resp->hwrm_intf_upd);
		device_printf(softc->dev, "Please update firmware with " 
		    "HWRM interface 1.0.0 or newer.\n");
        }
	if ((resp->hwrm_intf_maj == 1 && resp->hwrm_intf_min >= 2) ||
	    (resp->hwrm_intf_maj > 1))
		softc->flags |= BNXT_FLAG_HWRM_120;

	snprintf(softc->fw_ver_str, BC_HWRM_STR_LEN, "bc %d.%d.%d rm %d.%d.%d",
		resp->hwrm_fw_maj, resp->hwrm_fw_min, resp->hwrm_fw_bld,
		resp->hwrm_intf_maj, resp->hwrm_intf_min, resp->hwrm_intf_upd);

	if (resp->max_req_win_len)
		softc->hwrm_max_req_len = le16toh(resp->max_req_win_len);
	if (resp->def_req_timeout)
		softc->hwrm_cmd_timeo = le16toh(resp->def_req_timeout);

	device_printf(softc->dev, "Version: %s\n", softc->fw_ver_str);

fail:
	BNXT_HWRM_UNLOCK(softc);
	return rc;
}

int
bnxt_hwrm_func_drv_rgtr(struct bnxt_softc *softc)
{
	struct hwrm_func_drv_rgtr_input req = {0};

	bnxt_hwrm_cmd_hdr_init(softc, &req, HWRM_FUNC_DRV_RGTR, -1, -1);

	req.enables = htole32(HWRM_FUNC_DRV_RGTR_INPUT_ENABLES_VER |
			      HWRM_FUNC_DRV_RGTR_INPUT_ENABLES_OS_TYPE);
	req.os_type = htole16(HWRM_FUNC_DRV_RGTR_INPUT_OS_TYPE_FREEBSD);

	req.ver_maj = __FreeBSD_version / 100000;
	req.ver_min = (__FreeBSD_version / 1000) % 100;
	req.ver_upd = (__FreeBSD_version / 100) % 10;

	return hwrm_send_message(softc, &req, sizeof(req));
}


int
bnxt_hwrm_func_drv_unrgtr(struct bnxt_softc *softc, bool shutdown)
{
	struct hwrm_func_drv_unrgtr_input req = {0};

	bnxt_hwrm_cmd_hdr_init(softc, &req, HWRM_FUNC_DRV_UNRGTR, -1, -1);
	if (shutdown == true)
		req.flags |= HWRM_FUNC_DRV_UNRGTR_INPUT_FLAGS_PREPARE_FOR_SHUTDOWN;
	return hwrm_send_message(softc, &req, sizeof(req));
}


static inline int
_is_valid_ether_addr(uint8_t *addr)
{
        char zero_addr[6] = { 0, 0, 0, 0, 0, 0 };
                
        if ((addr[0] & 1) || (!bcmp(addr, zero_addr, ETHER_ADDR_LEN)))
                return (FALSE);
        
        return (TRUE);
}
        
static inline void
get_random_ether_addr(uint8_t *addr)
{
	uint8_t temp[ETHER_ADDR_LEN];

	arc4rand(&temp, sizeof(temp), 0);
	temp[0] &= 0xFE;
	temp[0] |= 0x02;
	bcopy(temp, addr, sizeof(temp));
}

int
bnxt_hwrm_func_qcaps(struct bnxt_softc *softc)
{
	int rc = 0;
	struct hwrm_func_qcaps_input req = {0};
	struct hwrm_func_qcaps_output *resp = (void *)softc->hwrm_cmd_resp.idi_vaddr;

	bnxt_hwrm_cmd_hdr_init(softc, &req, HWRM_FUNC_QCAPS, -1, -1);
	req.fid = htole16(0xffff);

	BNXT_HWRM_LOCK(softc);
	rc = _hwrm_send_message(softc, &req, sizeof(req));
	if (rc)
		goto fail;

	if (BNXT_PF(softc)) {
		struct bnxt_pf_info *pf = &softc->pf;

		pf->fw_fid = le16toh(resp->fid);
		pf->port_id = le16toh(resp->port_id);
		memcpy(pf->mac_addr, resp->mac_address, ETHER_ADDR_LEN);
		pf->max_rsscos_ctxs = le16toh(resp->max_rsscos_ctx);
		pf->max_cp_rings = le16toh(resp->max_cmpl_rings);
		pf->max_tx_rings = le16toh(resp->max_tx_rings);
		pf->max_rx_rings = le16toh(resp->max_rx_rings);
		pf->max_hw_ring_grps = le32toh(resp->max_hw_ring_grps);
		if (!pf->max_hw_ring_grps)
			pf->max_hw_ring_grps = pf->max_tx_rings;
		pf->max_l2_ctxs = le16toh(resp->max_l2_ctxs);
		pf->max_vnics = le16toh(resp->max_vnics);
		pf->max_stat_ctxs = le16toh(resp->max_stat_ctx);
		pf->first_vf_id = le16toh(resp->first_vf_id);
		pf->max_vfs = le16toh(resp->max_vfs);
		pf->max_encap_records = le32toh(resp->max_encap_records);
		pf->max_decap_records = le32toh(resp->max_decap_records);
		pf->max_tx_em_flows = le32toh(resp->max_tx_em_flows);
		pf->max_tx_wm_flows = le32toh(resp->max_tx_wm_flows);
		pf->max_rx_em_flows = le32toh(resp->max_rx_em_flows);
		pf->max_rx_wm_flows = le32toh(resp->max_rx_wm_flows);
	} else {
		struct bnxt_vf_info *vf = &softc->vf;

		vf->fw_fid = le16toh(resp->fid);
		memcpy(vf->mac_addr, resp->mac_address, ETHER_ADDR_LEN);
		if (!_is_valid_ether_addr(vf->mac_addr))
			get_random_ether_addr(vf->mac_addr);

		vf->max_rsscos_ctxs = le16toh(resp->max_rsscos_ctx);
		vf->max_cp_rings = le16toh(resp->max_cmpl_rings);
		vf->max_tx_rings = le16toh(resp->max_tx_rings);
		vf->max_rx_rings = le16toh(resp->max_rx_rings);
		vf->max_l2_ctxs = le16toh(resp->max_l2_ctxs);
		vf->max_vnics = le16toh(resp->max_vnics);
		vf->max_stat_ctxs = le16toh(resp->max_stat_ctx);
	}

fail:
	BNXT_HWRM_UNLOCK(softc);
	return rc;
}

int
bnxt_hwrm_func_reset(struct bnxt_softc *softc)
{
	struct hwrm_func_reset_input req = {0};

	bnxt_hwrm_cmd_hdr_init(softc, &req, HWRM_FUNC_RESET, -1, -1);
	req.enables = 0;

	return hwrm_send_message(softc, &req, sizeof(req));
}

static void
bnxt_hwrm_set_link_common(struct bnxt_softc *softc,
			  struct hwrm_port_phy_cfg_input *req)
{
	uint8_t autoneg = softc->link_info.autoneg;
	uint16_t fw_link_speed = softc->link_info.req_link_speed;
	uint32_t advertising = softc->link_info.advertising;

	if (autoneg & BNXT_AUTONEG_SPEED) {
		req->auto_mode |=
			HWRM_PORT_PHY_CFG_INPUT_AUTO_MODE_SPEED_MASK;

		req->enables |= htole32(
			HWRM_PORT_PHY_CFG_INPUT_ENABLES_AUTO_LINK_SPEED_MASK);
		req->auto_link_speed_mask = htole16(advertising);

		req->enables |= htole32(HWRM_PORT_PHY_CFG_INPUT_ENABLES_AUTO_MODE);
		req->flags |=
			htole32(HWRM_PORT_PHY_CFG_INPUT_FLAGS_RESTART_AUTONEG);
	} else {
		req->force_link_speed = htole16(fw_link_speed);
		req->flags |= htole32(HWRM_PORT_PHY_CFG_INPUT_FLAGS_FORCE);
	}

	/* tell chimp that the setting takes effect immediately */
	req->flags |= htole32(HWRM_PORT_PHY_CFG_INPUT_FLAGS_RESET_PHY);
}


static void
bnxt_hwrm_set_pause_common(struct bnxt_softc *softc, struct hwrm_port_phy_cfg_input *req)
{
	if (softc->link_info.autoneg & BNXT_AUTONEG_FLOW_CTRL) {
		if (softc->flags & BNXT_FLAG_HWRM_120)
			req->auto_pause =
			    HWRM_PORT_PHY_CFG_INPUT_AUTO_PAUSE_AUTONEG_PAUSE;
		if (softc->link_info.req_flow_ctrl & BNXT_LINK_PAUSE_RX)
			req->auto_pause |= HWRM_PORT_PHY_CFG_INPUT_AUTO_PAUSE_RX;
		if (softc->link_info.req_flow_ctrl & BNXT_LINK_PAUSE_TX)
			req->auto_pause |= HWRM_PORT_PHY_CFG_INPUT_AUTO_PAUSE_RX;
		req->enables |=
			htole32(HWRM_PORT_PHY_CFG_INPUT_ENABLES_AUTO_PAUSE);
	} else {
		if (softc->link_info.req_flow_ctrl & BNXT_LINK_PAUSE_RX)
			req->force_pause |= HWRM_PORT_PHY_CFG_INPUT_FORCE_PAUSE_RX;
		if (softc->link_info.req_flow_ctrl & BNXT_LINK_PAUSE_TX)
			req->force_pause |= HWRM_PORT_PHY_CFG_INPUT_FORCE_PAUSE_TX;
		req->enables |=
			htole32(HWRM_PORT_PHY_CFG_INPUT_ENABLES_FORCE_PAUSE);
		if (softc->flags & BNXT_FLAG_HWRM_120) {
			req->auto_pause = req->force_pause;
			req->enables |= htole32(
			        HWRM_PORT_PHY_CFG_INPUT_ENABLES_AUTO_PAUSE);
		}
	}
}


//JFV this needs interface connection
static void
bnxt_hwrm_set_eee(struct bnxt_softc *softc, struct hwrm_port_phy_cfg_input *req)
{
        //struct ethtool_eee *eee = &softc->eee;
	bool	eee_enabled = false;

	if (eee_enabled) {
#if 0
		uint16_t eee_speeds;
		uint32_t flags = HWRM_PORT_PHY_CFG_INPUT_FLAGS_EEE_ENABLE;

		if (eee->tx_lpi_enabled)
			flags |= HWRM_PORT_PHY_CFG_INPUT_FLAGS_EEE_TX_LPI;

		req->flags |= htole32(flags);
		eee_speeds = bnxt_get_fw_auto_link_speeds(eee->advertised);
		req->eee_link_speed_mask = htole16(eee_speeds);
		req->tx_lpi_timer = htole32(eee->tx_lpi_timer);
#endif
	} else {
		req->flags |= htole32(HWRM_PORT_PHY_CFG_INPUT_FLAGS_EEE_DISABLE);
	}
}


int
bnxt_hwrm_set_link_setting(struct bnxt_softc *softc, bool set_pause, bool set_eee)
{
	struct hwrm_port_phy_cfg_input req = {0};

	bnxt_hwrm_cmd_hdr_init(softc, &req, HWRM_PORT_PHY_CFG, -1, -1);
	if (set_pause)
		bnxt_hwrm_set_pause_common(softc, &req);

	bnxt_hwrm_set_link_common(softc, &req);
	if (set_eee)
		bnxt_hwrm_set_eee(softc, &req);
	return hwrm_send_message(softc, &req, sizeof(req));
}


int
bnxt_hwrm_set_pause(struct bnxt_softc *softc)
{
	struct hwrm_port_phy_cfg_input req = {0};
	int rc;

	bnxt_hwrm_cmd_hdr_init(softc, &req, HWRM_PORT_PHY_CFG, -1, -1);
	bnxt_hwrm_set_pause_common(softc, &req);

	if ((softc->link_info.autoneg & BNXT_AUTONEG_FLOW_CTRL) ||
	    softc->link_info.force_link_chng)
		bnxt_hwrm_set_link_common(softc, &req);

	BNXT_HWRM_LOCK(softc);
	rc = _hwrm_send_message(softc, &req, sizeof(req));
	if (!rc && !(softc->link_info.autoneg & BNXT_AUTONEG_FLOW_CTRL)) {
		/* since changing of pause setting doesn't trigger any link
		 * change event, the driver needs to update the current pause
		 * result upon successfully return of the phy_cfg command */
		softc->link_info.pause =
		softc->link_info.force_pause = softc->link_info.req_flow_ctrl;
		softc->link_info.auto_pause = 0;
		if (!softc->link_info.force_link_chng)
			bnxt_report_link(softc);
	}
	softc->link_info.force_link_chng = false;
	BNXT_HWRM_UNLOCK(softc);
	return rc;
}

int
bnxt_hwrm_vnic_set_rss(struct bnxt_softc *softc, uint16_t vnic_id, bool set_rss)
{
	struct bnxt_vnic_info *vnic = &softc->vnic_info[vnic_id];
	struct hwrm_vnic_rss_cfg_input req = {0};
	uint16_t *rss_table = (uint16_t *)vnic->rss.idi_vaddr;
	uint32_t i, j, max_rings;

	if ((vnic->fw_rss_cos_lb_ctx == (uint16_t)HWRM_NA_SIGNATURE) ||
	    (!rss_table))
		return 0;

	bnxt_hwrm_cmd_hdr_init(softc, &req, HWRM_VNIC_RSS_CFG, -1, -1);
	if (set_rss) {
		vnic->hash_type = HWRM_VNIC_RSS_CFG_INPUT_HASH_TYPE_IPV4 |
				 HWRM_VNIC_RSS_CFG_INPUT_HASH_TYPE_TCP_IPV4 |
				 HWRM_VNIC_RSS_CFG_INPUT_HASH_TYPE_IPV6 |
				 HWRM_VNIC_RSS_CFG_INPUT_HASH_TYPE_TCP_IPV6;

		req.hash_type = htole32(vnic->hash_type);

		if (vnic->flags & BNXT_VNIC_RSS_FLAG)
			max_rings = softc->scctx->isc_nrxqsets;
		else
			max_rings = 1;

		/* Fill the RSS indirection table with ring group ids */
		for (i = 0, j = 0; i < HW_HASH_INDEX_SIZE; i++, j++) {
			if (j == max_rings)
				j = 0;
			rss_table[i] = htole16(vnic->fw_grp_ids[j]);
		}
		req.ring_grp_tbl_addr = htole64(vnic->rss.idi_paddr);
		req.hash_key_tbl_addr = htole64(vnic->rss.idi_paddr + vnic->rss_size);
	}
	req.rss_ctx_idx = htole16(vnic->fw_rss_cos_lb_ctx);
	return hwrm_send_message(softc, &req, sizeof(req));
}


int
bnxt_hwrm_vnic_set_hds(struct bnxt_softc *softc, uint16_t vnic_id)
{
	struct bnxt_vnic_info *vnic = &softc->vnic_info[vnic_id];
	struct hwrm_vnic_plcmodes_cfg_input req = {0};
	struct hwrm_vnic_plcmodes_cfg_output *resp;

	resp = (void *)softc->hwrm_cmd_resp.idi_vaddr;
	bnxt_hwrm_cmd_hdr_init(softc, &req, HWRM_VNIC_PLCMODES_CFG, -1, -1);
	req.flags = htole32(HWRM_VNIC_PLCMODES_CFG_INPUT_FLAGS_JUMBO_PLACEMENT |
				HWRM_VNIC_PLCMODES_CFG_INPUT_FLAGS_HDS_IPV4 |
				HWRM_VNIC_PLCMODES_CFG_INPUT_FLAGS_HDS_IPV6);

	req.vnic_id = htole32(vnic->fw_vnic_id);
	return hwrm_send_message(softc, &req, sizeof(req));
}


int
bnxt_hwrm_vnic_ctx_alloc(struct bnxt_softc *softc, uint16_t vnic_id)
{
	struct hwrm_vnic_rss_cos_lb_ctx_alloc_input req = {0};
	struct hwrm_vnic_rss_cos_lb_ctx_alloc_output *resp;
	int rc;

	resp = (void *)softc->hwrm_cmd_resp.idi_vaddr;

	bnxt_hwrm_cmd_hdr_init(softc, &req,
	    HWRM_VNIC_RSS_COS_LB_CTX_ALLOC, -1, -1);

	BNXT_HWRM_LOCK(softc);
	rc = _hwrm_send_message(softc, &req, sizeof(req));
	if (rc)
		goto fail;

	softc->vnic_info[vnic_id].fw_rss_cos_lb_ctx =
	    le16toh(resp->rss_cos_lb_ctx_id);

fail:
	BNXT_HWRM_UNLOCK(softc);
	return (rc);
}


int
bnxt_hwrm_vnic_cfg(struct bnxt_softc *softc, uint16_t vnic_id)
{
	int grp_idx = 0;
	struct bnxt_vnic_info *vnic = &softc->vnic_info[vnic_id];
	struct hwrm_vnic_cfg_input req = {0};
	struct hwrm_vnic_cfg_output *resp;

	resp = (void *)softc->hwrm_cmd_resp.idi_vaddr;
	bnxt_hwrm_cmd_hdr_init(softc, &req, HWRM_VNIC_CFG, -1, -1);

	/* Only RSS support for now */
	req.enables = htole32(HWRM_VNIC_CFG_INPUT_ENABLES_DFLT_RING_GRP |
				  HWRM_VNIC_CFG_INPUT_ENABLES_RSS_RULE);

#if 0
	if (vnic->flags & BNXT_VNIC_RSS_FLAG)
		grp_idx = 0;
	else if (vnic->flags & BNXT_VNIC_RFS_FLAG)
		grp_idx = vnic_id - 1;
#endif

	req.vnic_id = htole16(vnic->fw_vnic_id);
	req.dflt_ring_grp = htole16(softc->grp_info[grp_idx].fw_grp_id);

	req.rss_rule = htole16(vnic->fw_rss_cos_lb_ctx);
	req.cos_rule = htole16(0xffff);
	req.lb_rule = htole16(0xffff);

	req.mru = htole16(iflib_get_ifp(softc->ctx)->if_mtu + ETHER_HDR_LEN +
	    ETHER_CRC_LEN + ETHER_VLAN_ENCAP_LEN);

	//JFV test - default func?
	req.flags = 1;

	if (softc->flags & BNXT_FLAG_STRIP_VLAN)
		req.flags |= htole32(HWRM_VNIC_CFG_INPUT_FLAGS_VLAN_STRIP_MODE);

	return hwrm_send_message(softc, &req, sizeof(req));
}

int
bnxt_hwrm_vnic_alloc(struct bnxt_softc *softc, uint16_t vnic_id,
		     uint16_t start_index, uint16_t num_rings)
{
	struct hwrm_vnic_alloc_input req = {0};
	struct hwrm_vnic_alloc_output *resp = (void *)softc->hwrm_cmd_resp.idi_vaddr;
	uint16_t	end_index = start_index + num_rings;
	int		rc = 0, i, j;

	/* map ring groups to this vnic */
	for (i = start_index, j = 0; i < end_index; i++, j++) {
		// JFV - is 'i' here the MSIX index as it should be??
		if (softc->grp_info[i].fw_grp_id ==
		    (uint16_t)HWRM_NA_SIGNATURE) {
			device_printf(softc->dev,
			    "Not enough ring groups avail:%x req:%x\n",
			    j, (end_index - start_index));
			break;
		}
		softc->vnic_info[vnic_id].fw_grp_ids[j] =
					softc->grp_info[i].fw_grp_id;
	}

	softc->vnic_info[vnic_id].fw_rss_cos_lb_ctx =
	    (uint16_t)HWRM_NA_SIGNATURE;
	if (vnic_id == 0) {
		req.flags = htole32(HWRM_VNIC_ALLOC_INPUT_FLAGS_DEFAULT);
		/* JFV - sort of arbitrary but this needs to be set */
		softc->vnic_info[vnic_id].flags = BNXT_VNIC_RSS_FLAG;
	}

	bnxt_hwrm_cmd_hdr_init(softc, &req, HWRM_VNIC_ALLOC, -1, -1);

	BNXT_HWRM_LOCK(softc);
	rc = _hwrm_send_message(softc, &req, sizeof(req));
	if (rc)
		goto fail;

	softc->vnic_info[vnic_id].fw_vnic_id = le32toh(resp->vnic_id);

fail:
	BNXT_HWRM_UNLOCK(softc);
	return (rc);
}


int
bnxt_hwrm_ring_grp_alloc(struct bnxt_softc *softc)
{
	int rc = 0;

	BNXT_HWRM_LOCK(softc);
	for (int i = 0; i < softc->scctx->isc_nrxqsets; i++) {
		struct hwrm_ring_grp_alloc_input req = {0};
		struct hwrm_ring_grp_alloc_output *resp;

		resp = (void *)softc->hwrm_cmd_resp.idi_vaddr;
		bnxt_hwrm_cmd_hdr_init(softc, &req, HWRM_RING_GRP_ALLOC,
		    -1, -1);
		req.cr = htole16(softc->grp_info[i].cp_ring_id);
		req.rr = htole16(softc->grp_info[i].rx_ring_id);
		req.ar = htole16(softc->grp_info[i].ag_ring_id);
		req.sc = htole16(softc->grp_info[i].stats_ctx);

		rc = _hwrm_send_message(softc, &req, sizeof(req));
		if (rc)
			break;

		softc->grp_info[i].fw_grp_id = le32toh(resp->ring_group_id);
	}
	BNXT_HWRM_UNLOCK(softc);
	return rc;
}

/*
 * Ring allocation message to the firmware
 */
int
bnxt_hwrm_ring_alloc(struct bnxt_softc *softc, uint8_t type,
    struct bnxt_ring *ring, uint16_t cmpl_ring_id, uint32_t stat_ctx_id)
{
	struct hwrm_ring_alloc_input req = {0};
	struct hwrm_ring_alloc_output *resp;
	int rc;

	resp = (void *)softc->hwrm_cmd_resp.idi_vaddr;
	bnxt_hwrm_cmd_hdr_init(softc, &req, HWRM_RING_ALLOC, -1, -1);
	req.enables = htole32(0);
	req.fbo = htole32(0);

	if (stat_ctx_id != HWRM_NA_SIGNATURE) {
		req.enables |= htole32(
		    HWRM_RING_ALLOC_INPUT_ENABLES_STAT_CTX_ID_VALID);
		req.stat_ctx_id = htole32(stat_ctx_id);
	}
	req.ring_type = type;
	req.page_tbl_addr = htole64(ring->paddr);
	req.length = htole32(ring->ring_size);
	req.logical_id = htole16(ring->id);
	req.cmpl_ring_id = htole16(cmpl_ring_id);
	req.queue_id = htole16(softc->q_info[0].id);
	req.int_mode = HWRM_RING_ALLOC_INPUT_INT_MODE_MSIX;
	BNXT_HWRM_LOCK(softc);
	rc = _hwrm_send_message(softc, &req, sizeof(req));
	if (rc)
		goto fail;

	ring->phys_id = le16toh(resp->ring_id);

fail:
	BNXT_HWRM_UNLOCK(softc);
	return rc;
}

int
bnxt_hwrm_ring_free(struct bnxt_softc *softc, uint8_t type, uint16_t phys_id)
{
	struct hwrm_ring_free_input req = {0};
	struct hwrm_ring_free_output *resp;

	resp = (void *)softc->hwrm_cmd_resp.idi_vaddr;
	bnxt_hwrm_cmd_hdr_init(softc, &req, HWRM_RING_FREE, -1, -1);

	req.ring_type = type;
	req.ring_id = htole16(phys_id);

	return hwrm_send_message(softc, &req, sizeof(req));
}

int
bnxt_hwrm_stat_ctx_alloc(struct bnxt_softc *softc, struct bnxt_cp_ring *cpr,
    uint64_t paddr)
{
	struct hwrm_stat_ctx_alloc_input req = {0};
	struct hwrm_stat_ctx_alloc_output *resp;
	int rc = 0;

	resp = (void *)softc->hwrm_cmd_resp.idi_vaddr;
	bnxt_hwrm_cmd_hdr_init(softc, &req, HWRM_STAT_CTX_ALLOC, -1, -1);

	req.update_period_ms = htole32(1000);
	req.stats_dma_addr = htole64(paddr);

	BNXT_HWRM_LOCK(softc);
	rc = _hwrm_send_message(softc, &req, sizeof(req));
	if (rc)
		goto fail;

	cpr->stats_ctx_id = le32toh(resp->stat_ctx_id);

fail:
	BNXT_HWRM_UNLOCK(softc);

	return rc;
}

int
bnxt_hwrm_stat_ctx_free(struct bnxt_softc *softc, struct bnxt_cp_ring *cpr)
{
	struct hwrm_stat_ctx_free_input req = {0};
	struct hwrm_stat_ctx_free_output *resp;

	resp = (void *)softc->hwrm_cmd_resp.idi_vaddr;
	bnxt_hwrm_cmd_hdr_init(softc, &req, HWRM_STAT_CTX_FREE, -1, -1);

	req.stat_ctx_id = htole16(cpr->stats_ctx_id);

	return hwrm_send_message(softc, &req, sizeof(req));
}

int
bnxt_hwrm_port_qstats(struct bnxt_softc *softc)
{
	struct bnxt_pf_info *pf = &softc->pf;
	struct hwrm_port_qstats_input req = {0};
	struct hwrm_port_qstats_output *resp;

	resp = (void *)softc->hwrm_cmd_resp.idi_vaddr;
	bnxt_hwrm_cmd_hdr_init(softc, &req, HWRM_PORT_QSTATS, -1, -1);

	req.port_id = pf->port_id;
	req.tx_stat_host_addr = htole64(softc->hw_tx_port_stats.idi_paddr);
	req.rx_stat_host_addr = htole64(softc->hw_rx_port_stats.idi_paddr);

	return hwrm_send_message(softc, &req, sizeof(req));
}


int
bnxt_hwrm_cfa_l2_set_rx_mask(struct bnxt_softc *softc, uint16_t vnic_id)
{
        struct hwrm_cfa_l2_set_rx_mask_input req = {0};
        struct bnxt_vnic_info *vnic = &softc->vnic_info[vnic_id];

        bnxt_hwrm_cmd_hdr_init(softc, &req, HWRM_CFA_L2_SET_RX_MASK, -1, -1);

        req.vnic_id = htole32(vnic->fw_vnic_id);

        req.num_mc_entries = htole32(vnic->mc_list_count);
        req.mc_tbl_addr = htole64(vnic->mc_list.idi_paddr);
        req.mask = htole32(vnic->rx_mask);
        return hwrm_send_message(softc, &req, sizeof(req));
}


int
bnxt_hwrm_set_filter(struct bnxt_softc *softc, struct bnxt_vnic_info *vnic,
		     struct bnxt_filter_info *filter)
{
	struct hwrm_cfa_l2_filter_alloc_input	req = {0};
	struct hwrm_cfa_l2_filter_alloc_output	*resp;
	uint32_t enables = 0;
	int rc = 0;

	resp = (void *)softc->hwrm_cmd_resp.idi_vaddr;
        bnxt_hwrm_cmd_hdr_init(softc, &req, HWRM_CFA_L2_FILTER_ALLOC, -1, -1);

        req.flags = htole32(filter->flags);

	enables = filter->enables | HWRM_CFA_L2_FILTER_ALLOC_INPUT_ENABLES_DST_ID;

	req.dst_id = htole16(vnic->fw_vnic_id);

        if (enables & HWRM_CFA_L2_FILTER_ALLOC_INPUT_ENABLES_L2_ADDR)
		memcpy(req.l2_addr, filter->l2_addr, ETHER_ADDR_LEN);

        if (enables & HWRM_CFA_L2_FILTER_ALLOC_INPUT_ENABLES_L2_ADDR_MASK)
		memcpy(req.l2_addr_mask, filter->l2_addr_mask, ETHER_ADDR_LEN);

        if (enables & HWRM_CFA_L2_FILTER_ALLOC_INPUT_ENABLES_L2_OVLAN)
		req.l2_ovlan = filter->l2_ovlan;

        if (enables & HWRM_CFA_L2_FILTER_ALLOC_INPUT_ENABLES_L2_OVLAN_MASK)
		req.l2_ovlan_mask = filter->l2_ovlan_mask;

	req.enables = htole32(enables);

	BNXT_HWRM_LOCK(softc);
	rc = _hwrm_send_message(softc, &req, sizeof(req));
	if (rc)
		goto fail;

	filter->fw_l2_filter_id = le64toh(resp->l2_filter_id);

fail:
	BNXT_HWRM_UNLOCK(softc);
	return (rc);
}


int
bnxt_hwrm_clear_filter(struct bnxt_softc *softc, struct bnxt_filter_info *filter)
{
	struct hwrm_cfa_l2_filter_free_input	req = {0};
	struct hwrm_cfa_l2_filter_free_output	*resp;
	int rc = 0;

	resp = (void *)softc->hwrm_cmd_resp.idi_vaddr;
	bnxt_hwrm_cmd_hdr_init(softc, &req, HWRM_CFA_L2_FILTER_FREE, -1, -1);

	req.l2_filter_id = htole64(filter->fw_l2_filter_id);

	rc = hwrm_send_message(softc, &req, sizeof(req));
	filter->fw_l2_filter_id = -1;

	return (rc);
}
