/*
 * Additional values not defined by other headers, they
 * seem a little incomplete.
 *
 * Find codes in the various SCSI specs.
 * BTW sense codes are at www.t10.org/lists/asc-num.txt
 *
 */
#pragma once

/*
 * SCSI Opcodes
 */
#define READ_FORMAT_CAPACITIES          0x23
#define UNMAP                           0x42
#define GET_CONFIGURATION               0x46
#define READ_DISC_INFORMATION           0x51
#define MODE_SELECT_10                  0x55
#define MODE_SENSE_10                   0x5a
#define EXTENDED_COPY                   0x83
#define RECEIVE_COPY_RESULTS            0x84 /* RECEIVE COPY STATUS */
#define READ_16                         0x88
#define COMPARE_AND_WRITE               0x89
#define WRITE_16                        0x8a
#define WRITE_VERIFY_16                 0x8e
#define SYNCHRONIZE_CACHE_16            0x91
#define WRITE_SAME_16                   0x93
#define SERVICE_ACTION_IN_16            0x9e
#define READ_DVD_STRUCTURE              0xad
#define MECHANISM_STATUS                0xbd
#define MAINTENANCE_IN			0xa3
#define MAINTENANCE_OUT			0xa4
#define MI_REPORT_TARGET_PGS		0x0a
#define MO_SET_TARGET_PGS		0x0a

/*
 * Receive Copy Results Sevice Actions
 */
#define RCR_SA_COPY_STATUS              0x00
#define RCR_SA_RECEIVE_DATA             0x01
#define RCR_SA_OPERATING_PARAMETERS     0x03
#define RCR_SA_FAILED_SEGMENT_DETAILS   0x04

/*
 * Receive Copy Results Operating Parameters
 */
#define RCR_OP_MAX_TARGET_DESC_COUNT    0x02
#define RCR_OP_MAX_SEGMENT_DESC_COUNT   0x01
#define RCR_OP_MAX_DESC_LIST_LEN        1024
#define RCR_OP_MAX_SEGMENT_LEN          16777216
#define RCR_OP_TOTAL_CONCURR_COPIES     0x01
#define RCR_OP_MAX_CONCURR_COPIES       0x01
#define RCR_OP_DATA_SEG_GRAN_LOG2       0x09
#define RCR_OP_INLINE_DATA_GRAN_LOG2    0x09
#define RCR_OP_HELD_DATA_GRAN_LOG2      0x09

/*
 * Receive Copy Results descriptor type codes supports
 */
#define RCR_OP_IMPLE_DES_LIST_LENGTH    0x02
#define XCOPY_SEG_DESC_TYPE_CODE_B2B    0x02 /* block --> block */
#define XCOPY_TARGET_DESC_TYPE_CODE_ID  0xe4 /* Identification descriptor */

/*
 * Service action opcodes
 */
#define READ_CAPACITY_16		0x10

/* SCSI protocols; these are taken from SPC-3 section 7.5 */
enum scsi_protocol {
	SCSI_PROTOCOL_FCP = 0,	/* Fibre Channel */
	SCSI_PROTOCOL_SPI = 1,	/* parallel SCSI */
	SCSI_PROTOCOL_SSA = 2,	/* Serial Storage Architecture - Obsolete */
	SCSI_PROTOCOL_SBP = 3,	/* firewire */
	SCSI_PROTOCOL_SRP = 4,	/* Infiniband RDMA */
	SCSI_PROTOCOL_ISCSI = 5,
	SCSI_PROTOCOL_SAS = 6,
	SCSI_PROTOCOL_ADT = 7,	/* Media Changers */
	SCSI_PROTOCOL_ATA = 8,
	SCSI_PROTOCOL_UNSPEC = 0xf, /* No specific protocol */
};

/*
 *  SCSI Architecture Model (SAM) Status codes. Taken from SAM-3 draft
 *  T10/1561-D Revision 4 Draft dated 7th November 2002.
 */
#define SAM_STAT_GOOD			0x00
#define SAM_STAT_CHECK_CONDITION	0x02
#define SAM_STAT_CONDITION_MET		0x04
#define SAM_STAT_BUSY			0x08
#define SAM_STAT_INTERMEDIATE		0x10
#define SAM_STAT_INTERMEDIATE_CONDITION_MET 0x14
#define SAM_STAT_RESERVATION_CONFLICT	0x18
#define SAM_STAT_COMMAND_TERMINATED	0x22        /* obsolete in SAM-3 */
#define SAM_STAT_TASK_SET_FULL		0x28
#define SAM_STAT_ACA_ACTIVE		0x30
#define SAM_STAT_TASK_ABORTED		0x40

#define ALUA_ACCESS_STATE_OPTIMIZED		0x0
#define ALUA_ACCESS_STATE_NON_OPTIMIZED		0x1
#define ALUA_ACCESS_STATE_STANDBY		0x2
#define ALUA_ACCESS_STATE_UNAVAILABLE		0x3
#define ALUA_ACCESS_STATE_LBA_DEPENDENT		0x4
#define ALUA_ACCESS_STATE_OFFLINE		0xe
#define ALUA_ACCESS_STATE_TRANSITIONING		0xf

#define ALUA_SUP_OPTIMIZED	0x01
#define ALUA_SUP_NON_OPTIMIZED	0x02
#define ALUA_SUP_STANDBY	0x04
#define ALUA_SUP_UNAVAILABLE	0x08
#define ALUA_SUP_LBA_DEPENDENT	0x10
#define ALUA_SUP_OFFLINE	0x40
#define ALUA_SUP_TRANSITIONING	0x80

#define TPGS_ALUA_NONE		0x00
#define TPGS_ALUA_IMPLICIT	0x10
#define TPGS_ALUA_EXPLICIT	0x20

#define ALUA_STAT_NONE				0x00
#define ALUA_STAT_ALTERED_BY_EXPLICIT_STPG	0x01
#define ALUA_STAT_ALTERED_BY_IMPLICIT_ALUA	0x02
