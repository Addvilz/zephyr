/*
 * Copyright (c) 2016 Intel Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#if defined(CONFIG_NET_DEBUG_L2_IEEE802154)
#define SYS_LOG_DOMAIN "net/ieee802154"
#define NET_LOG_ENABLED 1
#endif

#include <net/net_core.h>

#include <errno.h>

#include <net/net_if.h>
#include <net/ieee802154_radio.h>
#include <net/ieee802154.h>

#include "ieee802154_frame.h"
#include "ieee802154_mgmt.h"

enum net_verdict ieee802154_handle_beacon(struct net_if *iface,
					  struct ieee802154_mpdu *mpdu)
{
	struct ieee802154_context *ctx = net_if_l2_data(iface);
	struct ieee802154_radio_api *radio =
		(struct ieee802154_radio_api *)iface->dev->driver_api;

	NET_DBG("Beacon received");

	if (!ctx->scan_ctx) {
		return NET_DROP;
	}

	if (!mpdu->beacon->sf.association) {
		return NET_DROP;
	}

	k_sem_take(&ctx->res_lock, K_FOREVER);

	ctx->scan_ctx->pan_id = mpdu->mhr.src_addr->plain.pan_id;
	ctx->scan_ctx->lqi = radio->get_lqi(iface->dev);

	if (mpdu->mhr.fs->fc.src_addr_mode == IEEE802154_ADDR_MODE_SHORT) {
		ctx->scan_ctx->len = IEEE802154_SHORT_ADDR_LENGTH;
		ctx->scan_ctx->short_addr =
			mpdu->mhr.src_addr->plain.addr.short_addr;
	} else {
		ctx->scan_ctx->len = IEEE802154_EXT_ADDR_LENGTH;
		sys_memcpy_swap(ctx->scan_ctx->addr,
				mpdu->mhr.src_addr->plain.addr.ext_addr,
				IEEE802154_EXT_ADDR_LENGTH);
	}

	net_mgmt_event_notify(NET_EVENT_IEEE802154_SCAN_RESULT, iface);

	k_sem_give(&ctx->res_lock);

	return NET_OK;
}

static int ieee802154_cancel_scan(uint32_t mgmt_request, struct net_if *iface,
				  void *data, size_t len)
{
	struct ieee802154_context *ctx = net_if_l2_data(iface);

	ARG_UNUSED(data);
	ARG_UNUSED(len);

	NET_DBG("Cancelling scan request");

	ctx->scan_ctx = NULL;

	return 0;
}

NET_MGMT_REGISTER_REQUEST_HANDLER(NET_REQUEST_IEEE802154_CANCEL_SCAN,
				  ieee802154_cancel_scan);

static int ieee802154_scan(uint32_t mgmt_request, struct net_if *iface,
			   void *data, size_t len)
{
	struct ieee802154_radio_api *radio =
		(struct ieee802154_radio_api *)iface->dev->driver_api;
	struct ieee802154_context *ctx = net_if_l2_data(iface);
	struct ieee802154_req_params *scan =
		(struct ieee802154_req_params *)data;
	struct net_buf *frag = NULL;
	struct net_buf *buf = NULL;
	uint8_t channel;
	int ret;

	NET_DBG("%s scan requested",
		mgmt_request == NET_REQUEST_IEEE802154_ACTIVE_SCAN ?
		"Active" : "Passive");

	if (ctx->scan_ctx) {
		return -EALREADY;
	}

	if (mgmt_request == NET_REQUEST_IEEE802154_ACTIVE_SCAN) {
		struct ieee802154_frame_params params;

		params.dst.short_addr = IEEE802154_BROADCAST_ADDRESS;
		params.dst.pan_id = IEEE802154_BROADCAST_PAN_ID;

		buf = ieee802154_create_mac_cmd_frame(
			iface, IEEE802154_CFI_BEACON_REQUEST, &params);
		if (!buf) {
			NET_DBG("Could not create Beacon Request");
			return -ENOBUFS;
		}

		frag = buf->frags;
		buf->frags = NULL;
	}

	ctx->scan_ctx = scan;
	ret = 0;

	radio->set_pan_id(iface->dev, IEEE802154_BROADCAST_PAN_ID);

	if (radio->start(iface->dev)) {
		NET_DBG("Could not start device");
		ret = -EIO;

		goto out;
	}

	/* ToDo: For now, we assume we are on 2.4Ghz
	 * (device will have to export capabilities) */
	for (channel = 11; channel <= 26; channel++) {
		if (IEEE802154_IS_CHAN_UNSCANNED(scan->channel_set, channel)) {
			continue;
		}

		scan->channel = channel;
		radio->set_channel(iface->dev, channel);

		/* Active scan sends a beacon request */
		if (mgmt_request == NET_REQUEST_IEEE802154_ACTIVE_SCAN) {
			net_nbuf_ref(buf);
			net_nbuf_ref(frag);
			net_buf_frag_insert(buf, frag);

			ret = ieee802154_radio_send(iface, buf);
			if (ret) {
				NET_DBG("Could not send Beacon Request (%d)",
					ret);
				break;
			}
		}

		/* Context aware sleep */
		k_sleep(scan->duration);

		if (!ctx->scan_ctx) {
			NET_DBG("Scan request cancelled");
			ret = -ECANCELED;

			break;
		}
	}

	/* Let's come back to context's settings */
	radio->set_pan_id(iface->dev, ctx->pan_id);
	radio->set_channel(iface->dev, ctx->channel);
out:
	ctx->scan_ctx = NULL;

	if (buf) {
		if (!buf->frags) {
			net_nbuf_unref(frag);
		}

		net_nbuf_unref(buf);
	}

	return ret;
}

NET_MGMT_REGISTER_REQUEST_HANDLER(NET_REQUEST_IEEE802154_PASSIVE_SCAN,
				  ieee802154_scan);
NET_MGMT_REGISTER_REQUEST_HANDLER(NET_REQUEST_IEEE802154_ACTIVE_SCAN,
				  ieee802154_scan);

enum net_verdict ieee802154_handle_mac_command(struct net_if *iface,
					       struct ieee802154_mpdu *mpdu)
{
	struct ieee802154_context *ctx = net_if_l2_data(iface);

	if (mpdu->command->cfi == IEEE802154_CFI_ASSOCIATION_RESPONSE) {
		if (mpdu->command->assoc_res.status !=
		    IEEE802154_ASF_SUCCESSFUL) {
			return NET_DROP;
		}

		ctx->associated = true;
		k_sem_give(&ctx->req_lock);

		return NET_OK;
	}

	if (mpdu->command->cfi == IEEE802154_CFI_DISASSOCIATION_NOTIFICATION) {
		if (mpdu->command->disassoc_note.reason !=
		    IEEE802154_DRF_COORDINATOR_WISH) {
			return NET_DROP;
		}

		if (ctx->associated) {
			/* ToDo: check src address vs coord ones and reject
			 * if they don't match
			 */
			ctx->associated = false;

			return NET_OK;
		}
	}

	NET_DBG("Drop MAC command, unsupported CFI: 0x%x",
		mpdu->command->cfi);

	return NET_DROP;
}

static int ieee802154_associate(uint32_t mgmt_request, struct net_if *iface,
				void *data, size_t len)
{
	struct ieee802154_radio_api *radio =
		(struct ieee802154_radio_api *)iface->dev->driver_api;
	struct ieee802154_context *ctx = net_if_l2_data(iface);
	struct ieee802154_req_params *req =
		(struct ieee802154_req_params *)data;
	struct ieee802154_frame_params params;
	struct ieee802154_command *cmd;
	struct net_buf *buf;
	int ret = 0;

	k_sem_take(&ctx->req_lock, K_FOREVER);

	params.dst.len = req->len;
	if (params.dst.len == IEEE802154_SHORT_ADDR_LENGTH) {
		params.dst.short_addr = req->short_addr;
	} else {
		params.dst.ext_addr = req->addr;
	}

	params.dst.pan_id = req->pan_id;
	params.pan_id = req->pan_id;

	/* Set channel first */
	if (radio->set_channel(iface->dev, req->channel)) {
		ret = -EIO;
		goto out;
	}

	buf = ieee802154_create_mac_cmd_frame(
		iface, IEEE802154_CFI_ASSOCIATION_REQUEST, &params);
	if (!buf) {
		ret = -ENOBUFS;
		goto out;
	}

	cmd = ieee802154_get_mac_command(buf);
	cmd->assoc_req.ci.dev_type = 0; /* RFD */
	cmd->assoc_req.ci.power_src = 0; /* ToDo: set right power source */
	cmd->assoc_req.ci.rx_on = 1; /* ToDo: that will depends on PM */
	cmd->assoc_req.ci.sec_capability = 0; /* ToDo: security support */
	cmd->assoc_req.ci.alloc_addr = 0; /* ToDo: handle short addr */

	ctx->associated = false;

	if (net_if_send_data(iface, buf)) {
		net_nbuf_unref(buf);
		ret = -EIO;
		goto out;
	}

	/* ToDo: current timeout is arbitrary */
	k_sem_take(&ctx->req_lock, K_SECONDS(1));

	if (ctx->associated) {
		ctx->channel = req->channel;
		ctx->pan_id = req->pan_id;

		ctx->coord_addr_len = req->len;
		if (ctx->coord_addr_len == IEEE802154_SHORT_ADDR_LENGTH) {
			ctx->coord.short_addr = req->short_addr;
		} else {
			memcpy(ctx->coord.ext_addr,
			       req->addr, IEEE802154_EXT_ADDR_LENGTH);
		}
	} else {
		ret = -EACCES;
	}

out:
	k_sem_give(&ctx->req_lock);

	return ret;
}

NET_MGMT_REGISTER_REQUEST_HANDLER(NET_REQUEST_IEEE802154_ASSOCIATE,
				  ieee802154_associate);

static int ieee802154_disassociate(uint32_t mgmt_request, struct net_if *iface,
				   void *data, size_t len)
{
	struct ieee802154_context *ctx = net_if_l2_data(iface);
	struct ieee802154_frame_params params;
	struct ieee802154_command *cmd;
	struct net_buf *buf;

	if (!ctx->associated) {
		return -EALREADY;
	}

	params.dst.pan_id = ctx->pan_id;
	params.dst.len = ctx->coord_addr_len;
	if (params.dst.len == IEEE802154_SHORT_ADDR_LENGTH) {
		params.dst.short_addr = ctx->coord.short_addr;
	} else {
		params.dst.ext_addr = ctx->coord.ext_addr;
	}

	params.pan_id = ctx->pan_id;

	buf = ieee802154_create_mac_cmd_frame(
		iface, IEEE802154_CFI_DISASSOCIATION_NOTIFICATION, &params);
	if (!buf) {
		return -ENOBUFS;
	}

	cmd = ieee802154_get_mac_command(buf);
	cmd->disassoc_note.reason = IEEE802154_DRF_DEVICE_WISH;

	if (net_if_send_data(iface, buf)) {
		net_nbuf_unref(buf);
		return -EIO;
	}

	ctx->associated = false;

	return 0;
}

NET_MGMT_REGISTER_REQUEST_HANDLER(NET_REQUEST_IEEE802154_DISASSOCIATE,
				  ieee802154_disassociate);

static int ieee802154_set_ack(uint32_t mgmt_request, struct net_if *iface,
			      void *data, size_t len)
{
	struct ieee802154_context *ctx = net_if_l2_data(iface);

	ARG_UNUSED(data);
	ARG_UNUSED(len);

	if (mgmt_request == NET_REQUEST_IEEE802154_SET_ACK) {
		ctx->ack_requested = true;
	} else if (mgmt_request == NET_REQUEST_IEEE802154_UNSET_ACK) {
		ctx->ack_requested = false;
	}

	return 0;
}

NET_MGMT_REGISTER_REQUEST_HANDLER(NET_REQUEST_IEEE802154_SET_ACK,
				  ieee802154_set_ack);

NET_MGMT_REGISTER_REQUEST_HANDLER(NET_REQUEST_IEEE802154_UNSET_ACK,
				  ieee802154_set_ack);

static int ieee802154_set_parameters(uint32_t mgmt_request,
				     struct net_if *iface,
				     void *data, size_t len)
{
	const struct ieee802154_radio_api *radio = iface->dev->driver_api;
	struct ieee802154_context *ctx = net_if_l2_data(iface);
	uint16_t value;
	int ret = 0;

	if (ctx->associated) {
		return -EBUSY;
	}

	if (len != sizeof(uint16_t)) {
		return -EINVAL;
	}

	value = *((uint16_t *) data);

	if (mgmt_request == NET_REQUEST_IEEE802154_SET_CHAN) {
		if (ctx->channel != value) {
			ret = radio->set_channel(iface->dev, value);
			if (!ret) {
				ctx->channel = value;
			}
		}
	} else if (mgmt_request == NET_REQUEST_IEEE802154_SET_PAN_ID) {
		if (ctx->pan_id != value) {
			ret = radio->set_pan_id(iface->dev, value);
			if (!ret) {
				ctx->pan_id = value;
			}
		}
	} else {
		if (ctx->coord.short_addr != value) {
			ret = radio->set_short_addr(iface->dev, value);
			if (!ret) {
				ctx->coord.short_addr = value;
			}
		}
	}

	return ret;
}

NET_MGMT_REGISTER_REQUEST_HANDLER(NET_REQUEST_IEEE802154_SET_CHAN,
				  ieee802154_set_parameters);

NET_MGMT_REGISTER_REQUEST_HANDLER(NET_REQUEST_IEEE802154_SET_PAN_ID,
				  ieee802154_set_parameters);

NET_MGMT_REGISTER_REQUEST_HANDLER(NET_REQUEST_IEEE802154_SET_SHORT_ADDR,
				  ieee802154_set_parameters);