/*
   Copyright The Overlaybd Authors

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

   scsi_helper is based on tcmu-runner/scsi.c(https://github.com/open-iscsi/tcmu-runner/blob/master/scsi.c)
   It is for handling iscsi mode_sense command, supporting setting write protect.
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
#include "scsi_helper.h"
#include "scsi_defs.h"
#include "libtcmu.h"
#include "libtcmu_common.h"

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
                                 size_t *used_len, bool sense_ten) {
    int ret;

    ret = handler->get(dev, *buf, alloc_len - *used_len);

    if (!sense_ten && (*used_len + ret >= 255))
        return -EINVAL;

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
 * based on tcmu-runner tcmu_emulate_mode_sense
 */
int emulate_mode_sense(struct tcmu_device *dev, uint8_t *cdb,
                       struct iovec *iovec, size_t iov_cnt, bool readonly) {
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

    buf = (uint8_t *)calloc(1, alloc_len);
    if (!buf)
        return TCMU_STS_NO_RESOURCE;

    orig_buf = buf;
    buf += used_len;

    /* Don't fill in device-specific parameter */
    /* This helper fn doesn't support sw write protect (SWP) */

    /* Don't report block descriptors */

    if (page_code == 0x3f) {
        for (i = 0; i < ARRAY_SIZE(modesense_handlers); i++) {
            ret = handle_mode_sense(dev, &modesense_handlers[i], &buf,
                                    alloc_len, &used_len, sense_ten);
            if (ret < 0)
                goto free_buf;
        }
    } else {
        ret = 0;

        for (i = 0; i < ARRAY_SIZE(modesense_handlers); i++) {
            if (page_code == modesense_handlers[i].page &&
                subpage_code == modesense_handlers[i].subpage) {
                ret = handle_mode_sense(dev, &modesense_handlers[i], &buf,
                                        alloc_len, &used_len, sense_ten);
                break;
            }
        }

        if (ret <= 0)
            goto free_buf;
    }

    if (sense_ten) {
        uint16_t *ptr = (uint16_t *)orig_buf;
        *ptr = htobe16(used_len - 2);
    } else {
        orig_buf[0] = used_len - 1;
    }


	// set write protect bit
	if (readonly) {
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
