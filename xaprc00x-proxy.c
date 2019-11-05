// SPDX-License-Identifier: GPL-2.0+
/**
 * @file xaprc00x_proxy.c
 * @brief Implementation of the host proxy for SCM. These functions
 *	are called by the USB system when a message is completed.
 */

 #include <linux/socket.h>
#include <linux/net.h>
#include <net/sock.h>
#include "scm.h"
#include "xaprc00x-proxy.h"
#include "xaprc00x-sockets.h"

static int get_proxy_socket_id(__u16 dev, __u8 sock_id)
{
	return (dev<<8) | sock_id;
}

static __u8 get_device_socket_id(int sock_id)
{
	return sock_id & 0xFF;
}

/**
 * xaprc00x_proxy_fill_ack_common - Fill common ACK fields
 *
 * @orig The header of the packet being responded to
 * @ack The ACK packet to populate
 *
 * Fills common fields common to all ACK transactions that can be known by
 * reading the original message header.
 */
static void xaprc00x_proxy_fill_ack_common(struct scm_packet_hdr *orig,
	struct scm_packet *ack)
{
	ack->hdr.opcode = SCM_OP_ACK;
	ack->hdr.msg_id = orig->msg_id;
	ack->hdr.payload_len = 0;
	ack->hdr.sock_id = orig->sock_id;
	ack->ack.orig_opcode = orig->opcode;
}

/**
 * xaprc00x_proxy_fill_ack_open - Fill open specific ACK
 *
 * @packet The packet being reponded to
 * @ack The ACK packet to populate
 * @ret The return code from the operation
 * @id The ID created by the operation
 *
 * Fills an ACK packet after an OPEN procedure. ID is ignored unless ret==0
 */
static void xaprc00x_proxy_fill_ack_open(struct scm_packet *packet,
	struct scm_packet *ack, int ret, u8 id)
{
	xaprc00x_proxy_fill_ack_common(&packet->hdr, ack);
	ack->hdr.payload_len = 1;
	switch (ret) {
	case 0:
		ack->ack.open = 0x00;
		ack->hdr.sock_id = id;
		break;
	case -EEXIST:
		ack->ack.open = 0x01;
		break;
	case -EINVAL:
		ack->ack.open = 0x02;
		break;
	default:
		ack->ack.open = 0x01;
		break;
	}
	ack->ack.connect = ret;
}

/**
 * xaprc00x_proxy_fill_ack_connect - Fill connect specific ACK
 *
 * @packet The packet being reponded to
 * @ack The ACK packet to populate
 * @ret The return code from the operation
 *
 * Fills an ACK packet after an CONNECT procedure.
 */
static void xaprc00x_proxy_fill_ack_connect(struct scm_packet *packet,
	struct scm_packet *ack, int ret)
{
	xaprc00x_proxy_fill_ack_common(&packet->hdr, ack);
	ack->hdr.payload_len = 1;
	switch (ret) {
	case 0:
		ack->ack.connect = 0x00;
		break;
	case -ECONNREFUSED:
		ack->ack.connect = 0x02;
		break;
	case -ENETUNREACH:
		ack->ack.connect = 0x03;
		break;
	case -ETIMEDOUT:
		ack->ack.connect = 0x04;
		break;
	default:
		ack->ack.connect = 0x01;
		break;
	}
}

/**
 * xaprc00x_proxy_process_open - Process an OPEN packet
 *
 * @packet The packet sent by the device
 * @dev The device ID requesting this operation
 * @ack The ACK packet to populate
 *
 * Performs an OPEN operation based on an incoming SCM packet.
 */
void xaprc00x_proxy_process_open(struct scm_packet *packet, u16 dev,
	struct scm_packet *ack)
{
	int ret;
	int family, type, protocol;
	struct scm_payload_open *payload;
	int id = get_proxy_socket_id(dev,0);

	/* Find the smallest unoccupied ID for this device */
	while (xaprc00x_socket_exists(id))
		id++;

	 payload = &packet->open;

	/* Translate the SCM parameters to ones the socket interface */
	if (payload->addr_family == SCM_FAM_IP) {
		family = PF_INET;
	} else if (payload->addr_family == SCM_FAM_IP6) {
		family = PF_INET6;
	} else {
		ret = -EINVAL;
		goto fill_ack;
	}

	if (payload->protocol == SCM_PROTO_TCP) {
		protocol = IPPROTO_TCP;
	} else if (payload->protocol == SCM_PROTO_UDP) {
		protocol = IPPROTO_UDP;
	} else {
		ret = -EINVAL;
		goto fill_ack;
	}

	if (payload->type == SCM_TYPE_STREAM) {
		type = SOCK_STREAM;
	} else if (payload->type == SCM_TYPE_DGRAM) {
		type = SOCK_DGRAM;
	} else {
		ret = -EINVAL;
		goto fill_ack;
	}

	ret = xaprc00x_socket_create(id, family, type, protocol);

fill_ack:
	/* If creation succeded return created ID without the device */
	xaprc00x_proxy_fill_ack_open(packet, ack, ret, get_device_socket_id(id));
}

/**
 * xaprc00x_proxy_process_connect - Process an CONNECT packet
 *
 * @packet The packet sent by the device
 * @dev The device ID requesting this operation
 * @ack The ACK packet to populate
 *
 * Performs an CONNECT operation based on an incoming SCM packet.
 */
void xaprc00x_proxy_process_connect(struct scm_packet *packet, u16 dev,
	struct scm_packet *ack)
{
	int ret;
	struct scm_payload_connect_ip *payload = &packet->connect;
	struct scm_packet_hdr *hdr = &packet->hdr;
	int id = get_proxy_socket_id(dev, hdr->sock_id);

	switch (payload->family) {
	case SCM_FAM_IP:
		ret = xaprc00x_socket_connect_in4(id, 
			(char *)&(payload->ip4.ip_addr), 4, payload->port, 0);
		break;
	case SCM_FAM_IP6:
		ret = xaprc00x_socket_connect_in6(id, 
			(char *)&(payload->ip6.ip_addr), 16, payload->port,
			payload->ip6.flow_info, payload->ip6.scope_id, 0);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	xaprc00x_proxy_fill_ack_connect(packet, ack, ret);
}

/**
 * xaprc00x_proxy_process_close - Process an CLOSE packet
 *
 * @packet The packet sent by the device
 * @dev The device ID requesting this operation
 * @ack The ACK packet to populate
 *
 * Performs an CLOSE operation based on an incoming SCM packet.
 */
void xaprc00x_proxy_process_close(struct scm_packet *packet, u16 dev,
	struct scm_packet *ack)
{
	struct scm_packet_hdr *hdr = &packet->hdr;
	int id = get_proxy_socket_id(dev, hdr->sock_id);

	xaprc00x_socket_close(id);

	/* Close ACKs do not contain status data. */
	xaprc00x_proxy_fill_ack_common(hdr,ack);
}

/**
 * xaprc00x_proxy_process_cmd - Process a command from USB
 *
 * @scm_packet The packet being processed
 * @len The length of the data being passed
 * @ack The ack structure to fill out on complete.
 *
 * Acts as an intermediary between USB and the socket interface. Ack will be
 * ignored if the command type does not reply (such as recieving an ACK).
 */
void xaprc00x_proxy_process_cmd(struct scm_packet *packet, int packet_len,
	u16 dev, struct scm_packet *ack)
{
	/* Sanity check the length against the packet definition */
	if (packet->hdr.payload_len+sizeof(struct scm_packet_hdr) != packet_len)
		return;

	switch (packet->hdr.opcode) {
	case SCM_OP_OPEN:
		xaprc00x_proxy_process_open(packet, dev, ack);
		break;
	case SCM_OP_CONNECT:
		xaprc00x_proxy_process_connect(packet, dev, ack);
		break;
	case SCM_OP_CLOSE:
		xaprc00x_proxy_process_close(packet, dev, ack);
		break;
	case SCM_OP_SHUTDOWN:
	case SCM_OP_TRANSMIT:
	case SCM_OP_ACK:
	case SCM_OP_ACKDATA:
	default:
		break;
	}
}