/*
 * Copyright (c) 2014 Red Hat, Inc.
 *
 * This file is licensed to you under your choice of the GNU Lesser
 * General Public License, version 2.1 or any later version (LGPLv2.1 or
 * later), or the Apache License 2.0.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
#include <sys/uio.h>
#include <string.h>
#include <scsi/scsi.h>
#include <endian.h>
#include <errno.h>

#include "libtcmu.h"
#include "libtcmu_priv.h"
#include "be_byteshift.h"
#include "scsi.h"

#include <photon/common/alog.h>

struct alua_grp {
	/* ALUA spec values */
	uint8_t state;
	uint8_t supported_states;
	uint8_t tpgs;
	uint8_t status;
	uint8_t implicit_trans_secs;
	bool pref;
	uint16_t id;

	/* LIO settings */
	char *name;
	unsigned nonop_delay_msecs;
	unsigned trans_delay_msecs;

	struct tcmu_device *dev;
	uint8_t num_tgt_ports;
	/* entry on list returned by lib */
	// struct list_node entry;
	// struct list_head tgt_ports;
};

struct tgt_port {
	uint16_t rel_port_id;
	uint8_t proto_id;
	char *wwn;

	/* LIO settings */
	char *fabric;
	bool enabled;
	/* configfs tpgt */
	uint16_t tpgt;

	struct alua_grp *grp;
	// /* entry on group's tgt_ports list */
	// struct list_node entry;
};

int tcmu_emulate_std_inquiry(
	struct tgt_port *port,
	uint8_t *cdb,
	struct iovec *iovec,
	size_t iov_cnt)
{
	uint8_t buf[36];

	memset(buf, 0, sizeof(buf));

	buf[2] = 0x05; /* SPC-3 */
	buf[3] = 0x02; /* response data format */

	/*
	 * A Third-Party Copy (3PC)
	 *
	 * Enable the XCOPY
	 */
	buf[5] = 0x08;
	if (port)
		buf[5] |= port->grp->tpgs;

	buf[7] = 0x02; /* CmdQue */

	memcpy(&buf[8], "LIO-ORG ", 8);
	memset(&buf[16], 0x20, 16);
	memcpy(&buf[16], "TCMU device", 11);
	memcpy(&buf[32], "0002", 4);
	buf[4] = 31; /* Set additional length to 31 */

	tcmu_memcpy_into_iovec(iovec, iov_cnt, buf, sizeof(buf));
	return TCMU_STS_OK;
}

/* This func from CCAN str/hex/hex.c. Public Domain */
bool char_to_hex(unsigned char *val, char c)
{
	if (c >= '0' && c <= '9') {
		*val = c - '0';
		return true;
	}
	if (c >= 'a' && c <= 'f') {
		*val = c - 'a' + 10;
		return true;
	}
	if (c >= 'A' && c <= 'F') {
		*val = c - 'A' + 10;
		return true;
	}
	return false;
}

int tcmu_emulate_evpd_inquiry(
	struct tcmu_device *dev,
	struct tgt_port *port,
	uint8_t *cdb,
	struct iovec *iovec,
	size_t iov_cnt)
{
	switch (cdb[2]) {
	case 0x0: /* Supported VPD pages */
	{
		char data[16];

		memset(data, 0, sizeof(data));

		/* data[1] (page code) already 0 */
		/*
		  *  spc4r22 7.7.13 The supported VPD page list shall contain
		  *  a list of all VPD page codes (see 7.7) implemented by the
		  *  logical unit in ascending order beginning with page code 00h
		  */
		data[4] = 0x00;
		data[5] = 0x80;
		data[6] = 0x83;
		data[7] = 0xb0;
		data[8] = 0xb1;
		data[9] = 0xb2;

		data[3] = 6;

		tcmu_memcpy_into_iovec(iovec, iov_cnt, data, sizeof(data));
		return TCMU_STS_OK;
	}
	break;
	case 0x80: /* Unit Serial Number */
	{
		char data[512];
		char *wwn;
		uint32_t len;

		memset(data, 0, sizeof(data));

		data[1] = 0x80;

		wwn = tcmu_cfgfs_dev_get_wwn(dev);
		if (!wwn)
			return TCMU_STS_HW_ERR;

		/*
		 * The maximum length of the unit_serial has limited
		 * to 254 Bytes in kernel, so here limit to 256 Bytes
		 * will be enough.
		 */
		len = snprintf(&data[4], 256, "%s", wwn);
		data[3] = len + 1;

		tcmu_memcpy_into_iovec(iovec, iov_cnt, data, sizeof(data));

		free(wwn);
		return TCMU_STS_OK;
	}
	break;
	case 0x83: /* Device identification */
	{
		char data[512];
		char *ptr, *p, *wwn;
		size_t len, used = 4;
		uint16_t *tot_len = (uint16_t*) &data[2];
		uint32_t padding;
		bool next;
		int i;

		memset(data, 0, sizeof(data));

		data[1] = 0x83;

		wwn = tcmu_cfgfs_dev_get_wwn(dev);
		if (!wwn)
			return TCMU_STS_HW_ERR;

		ptr = &data[4];

		/* 1/5: T10 Vendor id */
		ptr[0] = 2; /* code set: ASCII */
		ptr[1] = 1; /* identifier: T10 vendor id */
		memcpy(&ptr[4], "LIO-ORG ", 8);
		len = snprintf(&ptr[12], sizeof(data) - 16, "%s", wwn);

		ptr[3] = 8 + len + 1;
		used += (uint8_t)ptr[3] + 4;
		ptr += (uint8_t)ptr[3] + 4;

		/* 2/5: NAA binary */
		ptr[0] = 1; /* code set: binary */
		ptr[1] = 3; /* identifier: NAA */
		ptr[3] = 16; /* body length for naa registered extended format */

		/*
		 * Set type 6 and use OpenFabrics IEEE Company ID: 00 14 05
		 */
		ptr[4] = 0x60;
		ptr[5] = 0x01;
		ptr[6] = 0x40;
		ptr[7] = 0x50;

		/*
		 * Fill in the rest with a binary representation of WWN
		 *
		 * This implementation only uses a nibble out of every byte of
		 * WWN, but this is what the kernel does, and it's nice for our
		 * values to match.
		 */
		next = true;
		for (p = wwn, i = 7; *p && i < 20; p++) {
			uint8_t val;

			if (!char_to_hex(&val, *p))
				continue;

			if (next) {
				next = false;
				ptr[i++] |= val;
			} else {
				next = true;
				ptr[i] = val << 4;
			}
		}

		used += 20;
		ptr += 20;

		/* 3/6: Vendor specific */
		ptr[0] = 2; /* code set: ASCII */
		ptr[1] = 0; /* identifier: vendor-specific */

		len = snprintf(&ptr[4], sizeof(data) - used - 4, "%s",
			       tcmu_dev_get_cfgstring(dev));
		ptr[3] = len + 1;

		used += (uint8_t)ptr[3] + 4;
		ptr += (uint8_t)ptr[3] + 4;

		if (!port)
			goto finish_page83;

		/* 4/5: Relative target port ID */
		ptr[0] = port->proto_id << 4; /* proto id */
		ptr[0] |= 0x1; /* Code set: binary */
		ptr[1] = 0x80; /* PIV set */
		ptr[1] |= 0x10; /* Association: 1b assoc with target port */
		ptr[1] |= 0x4; /* Designator type: Relative target port ID */
		ptr[3] = 4;
		/* rel tgt port ID */
		ptr[6] = (port->rel_port_id >> 8) & 0xff;
		ptr[7] = port->rel_port_id & 0xff;
		used += 8;
		ptr += 8;

		/* 5/5: Target port group */
		ptr[0] = port->proto_id << 4; /* proto id */
		ptr[0] |= 0x1; /* Code set: binary */
		ptr[1] = 0x80; /* PIV set */
		ptr[1] |= 0x10; /* Association: 1b assoc with target port */
		ptr[1] |= 0x5; /* Designator type: target port group */
		ptr[3] = 4;
		/* tpg id */
		ptr[6] = (port->grp->id >> 8) & 0xff;
		ptr[7] = port->grp->id & 0xff;
		used += 8;
		ptr += 8;

		/* SCSI name */
		ptr[0] = port->proto_id << 4;
		ptr[0] |= 0x3;	/* CODE SET = UTF-8 */
		ptr[1] = 0x80;	/* PIV=1 */
		ptr[1] |= 0x10; /* ASSOCIATION = target port: 01b */
		ptr[1] |= 0x8;	/* DESIGNATOR TYPE = SCSI name string */
		len = snprintf(&ptr[4], sizeof(data) - used - 4, "%s,t,0x%04x", port->wwn, port->tpgt);
		len += 1;		/* Include  NULL terminator */
		/*
		 * The null-terminated, null-padded (see 4.4.2) SCSI
		 * NAME STRING field contains a UTF-8 format string.
		 * The number of bytes in the SCSI NAME STRING field
		 * (i.e., the value in the DESIGNATOR LENGTH field)
		 * shall be no larger than 256 and shall be a multiple
		 * of four.
		 */
		padding = ((-len) & 3);
		if (padding)
			len += padding;
		if (len > 256)
			len=256;
		ptr[3] = len;
		used += len + 4;
		ptr += len + 4;

		/* Target device designator */
		ptr[0] = port->proto_id << 4;
		ptr[0] |= 0x3;	/* CODE SET = UTF-8 */
		ptr[1] = 0x80;	/* PIV=1 */
		ptr[1] |= 0x20;	/* ASSOCIATION = target device: 10b */
		ptr[1] |= 0x8;	/* DESIGNATOR TYPE = SCSI name string */
		len = snprintf(&ptr[4], sizeof(data) - used -4, "%s", port->wwn);
		len +=1;		/* Include  NULL terminator */
		/*
		 * The null-terminated, null-padded (see 4.4.2) SCSI
		 * NAME STRING field contains a UTF-8 format string.
		 * The number of bytes in the SCSI NAME STRING field
		 * (i.e., the value in the DESIGNATOR LENGTH field)
		 * shall be no larger than 256 and shall be a multiple
		 * of four.
		 */
		padding = ((-len) & 3);
		if (padding)
			len += padding;
		if (len >256)
			len = 256;
		ptr[3] = len;
		used += len + 4;

finish_page83:
		/* Done with descriptor list */

		*tot_len = htobe16(used - 4);

		tcmu_memcpy_into_iovec(iovec, iov_cnt, data, used);

		free(wwn);
		wwn = NULL;

		return TCMU_STS_OK;
	}
	break;
	case 0xb0: /* Block Limits */
	{
		char data[64];
		uint32_t max_xfer_length;
		uint32_t unmap_gran_align;
		uint16_t val16;
		uint32_t val32;
		uint64_t val64;

		memset(data, 0, sizeof(data));

		data[1] = 0xb0;

		val16 = htobe16(0x3c);
		memcpy(&data[2], &val16, 2);

		/* WSNZ = 1: the device server won't support a value of zero
		 * in the NUMBER OF LOGICAL BLOCKS field in the WRITE SAME
		 * command CDBs
		 */
		data[4] = 0x01;

		/*
		 * From SCSI Commands Reference Manual, section Block Limits
		 * VPD page (B0h)
		 *
		 * MAXIMUM COMPARE AND WRITE LENGTH: set to a non-zero value
		 * indicates the maximum value that the device server accepts
		 * in the NUMBER OF LOGICAL BLOCKS field in the COMPARE AND
		 * WRITE command.
		 *
		 * It should be less than or equal to MAXIMUM TRANSFER LENGTH.
		 */
		data[5] = MAX_CAW_LENGTH;

		/*
		 * Daemons like runner may override the user requested
		 * value due to device specific limits.
		 */
		max_xfer_length = tcmu_dev_get_max_xfer_len(dev);

		val32 = htobe32(max_xfer_length);
		/* Max xfer length */
		memcpy(&data[8], &val32, 4);
		/* Optimal xfer length */
		memcpy(&data[12], &val32, 4);

		if (tcmu_dev_get_unmap_enabled(dev)) {
			/* MAXIMUM UNMAP LBA COUNT */
			val32 = htobe32(tcmu_dev_get_max_unmap_len(dev));
			memcpy(&data[20], &val32, 4);

			/* MAXIMUM UNMAP BLOCK DESCRIPTOR COUNT */
			val32 = htobe32(VPD_MAX_UNMAP_BLOCK_DESC_COUNT);
			memcpy(&data[24], &val32, 4);

			/* OPTIMAL UNMAP GRANULARITY */
			val32 = htobe32(tcmu_dev_get_opt_unmap_gran(dev));
			memcpy(&data[28], &val32, 4);

			/* UNMAP GRANULARITY ALIGNMENT */
			unmap_gran_align = tcmu_dev_get_unmap_gran_align(dev);
			val32 = htobe32(unmap_gran_align);
			memcpy(&data[32], &val32, 4);

			/* UGAVALID: An unmap granularity alignment valid bit */
			data[32] |= 0x80;
		}

		/* MAXIMUM WRITE SAME LENGTH */
		val64 = htobe64(VPD_MAX_WRITE_SAME_LENGTH);
		memcpy(&data[36], &val64, 8);

		tcmu_memcpy_into_iovec(iovec, iov_cnt, data, sizeof(data));

		return TCMU_STS_OK;
	}
	break;
	case 0xb1: /* Block Device Characteristics VPD page */
	{
		char data[64];
		uint16_t val16;

		memset(data, 0, sizeof(data));

		/*
		 * From spc-5 Revision 14, section 6.7.2 Standard INQUIRY data
		 * set the devive type to Direct access block device.
		 */
		data[0] = 0x00;

		/* PAGE CODE (B1h) */
		data[1] = 0xb1;

		/* PAGE LENGTH (003Ch)*/
		val16 = htobe16(0x003c);
		memcpy(&data[2], &val16, 2);

		if (tcmu_dev_get_solid_state_media(dev)) {
			val16 = htobe16(0x0001);
			memcpy(&data[4], &val16, 2);
		}

		tcmu_memcpy_into_iovec(iovec, iov_cnt, data, sizeof(data));
		return TCMU_STS_OK;
	}
	break;
	case 0xb2: /* Logical Block Provisioning VPD page */
	{
		char data[64];
		uint16_t val16;

		memset(data, 0, sizeof(data));

		/*
		 * From spc-5 Revision 14, section 6.7.2 Standard INQUIRY data
		 * set the device type to Direct access block device.
		 */
		data[0] = 0x00;

		/* PAGE CODE (B2h) */
		data[1] = 0xb2;

		/*
		 * PAGE LENGTH field: PROVISIONING GROUP DESCRIPTOR field will be
		 * not present.
		 */
		val16 = htobe16(0x0004);
		memcpy(&data[2], &val16, 2);

		/*
		 * The logical block provisioning read zeros (LBPRZ) field.
		 *
		 * The logical block data represented by unmapped LBAs is set to zeros
		 */
		data[5] = 0x04;

		/*
		 * The logical block provisioning unmap (LBPU|LBPWS|LBPWS10) fields.
		 *
		 * This will enable the UNMAP command for the device server and write
		 * same(10|16) command.
		 */
		if (tcmu_dev_get_unmap_enabled(dev))
			data[5] |= 0xe0;

		tcmu_memcpy_into_iovec(iovec, iov_cnt, data, sizeof(data));
		return TCMU_STS_OK;
	}
	break;
	default:
		LOG_ERROR("[dev `] Vital product data page code ` not support", dev->tcm_dev_name,
			     cdb[2]);
		return TCMU_STS_INVALID_CDB;
	}
}

/*
 * Emulate INQUIRY(0x12)
 */
int tcmu_emulate_inquiry(
	struct tcmu_device *dev,
	struct tgt_port *port,
	uint8_t *cdb,
	struct iovec *iovec,
	size_t iov_cnt)
{
	if (!(cdb[1] & 0x01)) {
		if (cdb[2])
			return TCMU_STS_INVALID_CDB;
		return tcmu_emulate_std_inquiry(port, cdb, iovec, iov_cnt);
	}
	return tcmu_emulate_evpd_inquiry(dev, port, cdb, iovec, iov_cnt);
}

int tcmu_emulate_test_unit_ready(
	uint8_t *cdb,
	struct iovec *iovec,
	size_t iov_cnt)
{
	return TCMU_STS_OK;
}

int tcmu_emulate_read_capacity_10(
	uint64_t num_lbas,
	uint32_t block_size,
	uint8_t *cdb,
	struct iovec *iovec,
	size_t iov_cnt)
{
	uint8_t buf[8];
	uint32_t val32;

	memset(buf, 0, sizeof(buf));

	if (num_lbas < 0x100000000ULL) {
		// Return the LBA of the last logical block, so subtract 1.
		val32 = htobe32(num_lbas-1);
	} else {
		// This lets the initiator know that he needs to use
		// Read Capacity(16).
		val32 = 0xffffffff;
	}

	memcpy(&buf[0], &val32, 4);

	val32 = htobe32(block_size);
	memcpy(&buf[4], &val32, 4);

	/* all else is zero */

	tcmu_memcpy_into_iovec(iovec, iov_cnt, buf, sizeof(buf));

	return TCMU_STS_OK;
}

int tcmu_emulate_read_capacity_16(
	uint64_t num_lbas,
	uint32_t block_size,
	uint8_t *cdb,
	struct iovec *iovec,
	size_t iov_cnt)
{
	uint8_t buf[32];
	uint64_t val64;
	uint32_t val32;

	memset(buf, 0, sizeof(buf));

	// Return the LBA of the last logical block, so subtract 1.
	val64 = htobe64(num_lbas-1);
	memcpy(&buf[0], &val64, 8);

	val32 = htobe32(block_size);
	memcpy(&buf[8], &val32, 4);

	/*
	 * Logical Block Provisioning Management Enabled (LBPME) bit
	 *
	 * The LBPME bit sets to one and then the logical unit implements
	 * logical block provisioning management
	 */
	buf[14] = 0x80;

	/*
	 * The logical block provisioning read zeros (LBPRZ) bit shall be
	 * set to one if the LBPRZ field is set to xx1b in VPD B2. The
	 * LBPRZ bit shall be set to zero if the LBPRZ field is not set
	 * to xx1b.
	 */
	buf[14] |= 0x40;

	/* all else is zero */

	tcmu_memcpy_into_iovec(iovec, iov_cnt, buf, sizeof(buf));

	return TCMU_STS_OK;
}

static void copy_to_response_buf(uint8_t *to_buf, size_t to_len,
				 uint8_t *from_buf, size_t from_len)
{
	if (!to_buf)
		return;
	/*
	 * SPC 4r37: 4.3.5.6 Allocation length:
	 *
	 * The device server shall terminate transfers to the Data-In Buffer
	 * when the number of bytes or blocks specified by the ALLOCATION
	 * LENGTH field have been transferred or when all available data
	 * have been transferred, whichever is less.
	 */
	memcpy(to_buf, from_buf, to_len > from_len ? from_len : to_len);
}

static int handle_rwrecovery_page(struct tcmu_device *dev, uint8_t *ret_buf,
			   size_t ret_buf_len)
{
	uint8_t buf[12];

	memset(buf, 0, sizeof(buf));
	buf[0] = 0x1;
	buf[1] = 0xa;

	copy_to_response_buf(ret_buf, ret_buf_len, buf, 12);
	return 12;
}

static int handle_cache_page(struct tcmu_device *dev, uint8_t *ret_buf,
		      size_t ret_buf_len)
{
	uint8_t buf[20];

	memset(buf, 0, sizeof(buf));
	buf[0] = 0x8;
	buf[1] = 0x12;

	/*
	 * If device supports a writeback cache then set writeback
	 * cache enable (WCE)
	 */
	if (tcmu_dev_get_write_cache_enabled(dev))
		buf[2] = 0x4;

	copy_to_response_buf(ret_buf, ret_buf_len, buf, 20);
	return 20;
}

static int handle_control_page(struct tcmu_device *dev, uint8_t *ret_buf,
			       size_t ret_buf_len)
{
	uint8_t buf[12];

	memset(buf, 0, sizeof(buf));
	buf[0] = 0x0a;
	buf[1] = 0x0a;

	/* From spc4r31, section 7.5.7 Control mode Page
	 *
	 * GLTSD = 1: because we don't implicitly save log parameters
	 *
	 * A global logging target save disable (GLTSD) bit set to
	 * zero specifies that the logical unit implicitly saves, at
	 * vendor specific intervals, each log parameter in which the
	 * TSD bit (see 7.3) is set to zero. A GLTSD bit set to one
	 * specifies that the logical unit shall not implicitly save
	 * any log parameters.
	 */
	buf[2] = 0x02;

	/* From spc4r31, section 7.5.7 Control mode Page
	 *
	 * TAS = 1: Currently not settable by tcmu. Using the LIO default
	 *
	 * A task aborted status (TAS) bit set to zero specifies that
	 * aborted commands shall be terminated by the device server
	 * without any response to the application client. A TAS bit
	 * set to one specifies that commands aborted by the actions
	 * of an I_T nexus other than the I_T nexus on which the command
	 * was received shall be completed with TASK ABORTED status
	 */
	buf[5] = 0x40;

	/* From spc4r31, section 7.5.7 Control mode Page
	 *
	 * BUSY TIMEOUT PERIOD: Currently is unlimited
	 *
	 * The BUSY TIMEOUT PERIOD field specifies the maximum time, in
	 * 100 milliseconds increments, that the application client allows
	 * for the device server to return BUSY status for unanticipated
	 * conditions that are not a routine part of commands from the
	 * application client. This value may be rounded down as defined
	 * in 5.4(the Parameter rounding section).
	 *
	 * A 0000h value in this field is undefined by this standard.
	 * An FFFFh value in this field is defined as an unlimited period.
	 */
	buf[8] = 0xff;
	buf[9] = 0xff;

	copy_to_response_buf(ret_buf, ret_buf_len, buf, 12);
	return 12;
}


static struct mode_sense_handler {
	uint8_t page;
	uint8_t subpage;
	int (*get)(struct tcmu_device *dev, uint8_t *buf, size_t buf_len);
} modesense_handlers[] = {
	{0x1, 0, handle_rwrecovery_page},
	{0x8, 0, handle_cache_page},
	{0xa, 0, handle_control_page},
};

static ssize_t handle_mode_sense(struct tcmu_device *dev,
				 struct mode_sense_handler *handler,
				 uint8_t **buf, size_t alloc_len,
				 size_t *used_len, bool sense_ten)
{
	int ret;

	ret = handler->get(dev, *buf, alloc_len - *used_len);

	if  (!sense_ten && (*used_len + ret >= 255))
		return -EINVAL;

	/*
	 * SPC 4r37: 4.3.5.6 Allocation length:
	 *
	 * If the information being transferred to the Data-In Buffer includes
	 * fields containing counts of the number of bytes in some or all of
	 * the data (e.g., the PARAMETER DATA LENGTH field, the PAGE LENGTH
	 * field, the DESCRIPTOR LENGTH field, the AVAILABLE DATA field),
	 * then the contents of these fields shall not be altered to reflect
	 * the truncation, if any, that results from an insufficient
	 * ALLOCATION LENGTH value
	 */
	/*
	 * Setup the buffer so to still loop over the handlers, but just
	 * increment the used_len so we can return the
	 * final value.
	 */
	if (*buf && (*used_len + ret >= alloc_len))
		*buf = NULL;

	*used_len += ret;
	if (*buf)
		*buf += ret;
	return ret;
}

/*
 * Handle MODE_SENSE(6) and MODE_SENSE(10).
 *
 * For TYPE_DISK only.
 */
int tcmu_emulate_mode_sense(
	struct tcmu_device *dev,
	uint8_t *cdb,
	struct iovec *iovec,
	size_t iov_cnt)
{
	bool sense_ten = (cdb[0] == MODE_SENSE_10);
	uint8_t page_code = cdb[2] & 0x3f;
	uint8_t subpage_code = cdb[3];
	size_t alloc_len = tcmu_cdb_get_xfer_length(cdb);
	int i;
	int ret;
	size_t used_len;
	uint8_t *buf;
	uint8_t *orig_buf = NULL;

	if (!alloc_len)
		return TCMU_STS_OK;

	/* Mode parameter header. Mode data length filled in at the end. */
	used_len = sense_ten ? 8 : 4;
	if (used_len > alloc_len)
		goto fail;

	buf = calloc(1, alloc_len);
	if (!buf)
		return TCMU_STS_NO_RESOURCE;

	orig_buf = buf;
	buf += used_len;

	/* Don't fill in device-specific parameter */
	/* This helper fn doesn't support sw write protect (SWP) */

	/* Don't report block descriptors */

	if (page_code == 0x3f) {
		for (i = 0; i < (int)ARRAY_SIZE(modesense_handlers); i++) {
			ret = handle_mode_sense(dev, &modesense_handlers[i],
						&buf, alloc_len, &used_len,
						sense_ten);
			if (ret < 0)
				goto free_buf;
		}
	} else {
		ret = 0;

		for (i = 0; i < (int)ARRAY_SIZE(modesense_handlers); i++) {
			if (page_code == modesense_handlers[i].page &&
			    subpage_code == modesense_handlers[i].subpage) {
				ret = handle_mode_sense(dev,
							&modesense_handlers[i],
							&buf, alloc_len,
							&used_len, sense_ten);
				break;
			}
		}

		if (ret <= 0)
			goto free_buf;
	}

	if (sense_ten) {
		uint16_t *ptr = (uint16_t*) orig_buf;
		*ptr = htobe16(used_len - 2);
	}
	else {
		orig_buf[0] = used_len - 1;
	}

	if (tcmu_dev_get_write_protect_enabled(dev)) {
		if (sense_ten) {
			orig_buf[3] |= 0x80;
		} else {
			orig_buf[2] |= 0x80;
		}
	}

	tcmu_memcpy_into_iovec(iovec, iov_cnt, orig_buf, alloc_len);
	free(orig_buf);
	return TCMU_STS_OK;

free_buf:
	free(orig_buf);
fail:
	return TCMU_STS_INVALID_CDB;
}

/*
 * Handle MODE_SELECT(6) and MODE_SELECT(10).
 *
 * For TYPE_DISK only.
 */
int tcmu_emulate_mode_select(
	struct tcmu_device *dev,
	uint8_t *cdb,
	struct iovec *iovec,
	size_t iov_cnt)
{
	bool select_ten = (cdb[0] == MODE_SELECT_10);
	uint8_t page_code = cdb[2] & 0x3f;
	uint8_t subpage_code = cdb[3];
	size_t alloc_len = tcmu_cdb_get_xfer_length(cdb);
	int i;
	int ret = 0;
	size_t hdr_len = select_ten ? 8 : 4;
	uint8_t buf[512];
	uint8_t in_buf[512];
	bool got_sense = false;

	if (!alloc_len)
		return TCMU_STS_OK;

	if (tcmu_memcpy_from_iovec(in_buf, sizeof(in_buf), iovec, iov_cnt) >= sizeof(in_buf))
		return TCMU_STS_INVALID_PARAM_LIST_LEN;

	/* Abort if !pf or sp */
	if (!(cdb[1] & 0x10) || (cdb[1] & 0x01))
		return TCMU_STS_INVALID_CDB;

	memset(buf, 0, sizeof(buf));
	for (i = 0; i < (int)ARRAY_SIZE(modesense_handlers); i++) {
		if (page_code == modesense_handlers[i].page
		    && subpage_code == modesense_handlers[i].subpage) {
			ret = modesense_handlers[i].get(dev, &buf[hdr_len],
							sizeof(buf) - hdr_len);
			if (ret <= 0)
				return TCMU_STS_INVALID_CDB;

			if  (!select_ten && (hdr_len + ret >= 255))
				return TCMU_STS_INVALID_CDB;

			got_sense = true;
			break;
		}
	}

	if (!got_sense)
		return TCMU_STS_INVALID_CDB;

	if (alloc_len < (hdr_len + ret))
		return TCMU_STS_INVALID_PARAM_LIST_LEN;

	/* Verify what was selected is identical to what sense returns, since we
	   don't support actually setting anything. */
	if (memcmp(&buf[hdr_len], &in_buf[hdr_len], ret))
		return TCMU_STS_INVALID_PARAM_LIST;

	return TCMU_STS_OK;
}

int tcmu_emulate_start_stop(struct tcmu_device *dev, uint8_t *cdb)
{
	if ((cdb[4] >> 4) & 0xf)
		return TCMU_STS_INVALID_CDB;

	/* Currently, we don't allow ejecting the medium, so we're
	 * ignoring the FBO_PREV_EJECT flag, but it may turn out that
	 * initiators do not handle this well, so we may have to change
	 * this behavior.
	 */

	if (!(cdb[4] & 0x01))
		return TCMU_STS_INVALID_CDB;

	return TCMU_STS_OK;
}
