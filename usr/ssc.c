/*
 * This handles any SCSI OP codes defined in the standards as 'STREAM'
 *
 * Copyright (C) 2005 - 2009 Mark Harvey markh794 at gmail dot com
 *                                mark_harvey at symantec dot com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * See comments in vtltape.c for a more complete version release...
 *
 */

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <dirent.h>
#include <syslog.h>
#include <ctype.h>
#include <inttypes.h>
#include <assert.h>
#include "be_byteshift.h"
#include "scsi.h"
#include "list.h"
#include "vtl_common.h"
#include "logging.h"
#include "vtllib.h"
#include "spc.h"
#include "ssc.h"
#include "vtltape.h"
#include "q.h"
#include "log.h"
#include "mode.h"

uint8_t last_cmd;
int current_state;

static struct allow_overwrite_state {
	char *desc;
} allow_overwrite_desc[] = {
	{ "Disabled", },
	{ "Current Position", },
	{ "Format", },
	{ "Opps, Invalid field in CDB", },
};

uint8_t ssc_allow_overwrite(struct scsi_cmd *cmd)
{
	uint8_t *cdb = cmd->scb;
	uint8_t allow_overwrite = cdb[2] & 0x0f;
	uint8_t partition = cdb[3];
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;
	uint8_t ret_stat = SAM_STAT_GOOD;
	uint64_t allow_overwrite_block;
	struct priv_lu_ssc *lu_ssc;

	lu_ssc = cmd->lu->lu_private;

	if (allow_overwrite > 2) /* Truncate bad values 3 to 15 -> '3' */
		allow_overwrite = 3;

	MHVTL_DBG(1, "ALLOW OVERWRITE (%ld) : %s **",
			(long)cmd->dbuf_p->serialNo,
			allow_overwrite_desc[allow_overwrite].desc);

	lu_ssc->allow_overwrite = FALSE;

	switch (allow_overwrite) {
	case 0:
		break;
	case 1:  /* current position */
		if (partition) { /* Paritions not supported at this stage */
			MHVTL_LOG("Partitions not implemented at this time");
			mkSenseBuf(ILLEGAL_REQUEST,
					E_INVALID_FIELD_IN_CDB,
					sam_stat);
			return SAM_STAT_CHECK_CONDITION;
		}
		allow_overwrite_block = get_unaligned_be64(&cdb[4]);
		MHVTL_DBG(1, "Allow overwrite block: %lld",
					(long long)allow_overwrite_block);
		if (allow_overwrite_block == current_tape_block()) {
			lu_ssc->allow_overwrite_block = allow_overwrite_block;
			lu_ssc->allow_overwrite = TRUE;
		} else {
			/* Set allow_overwrite position to an invalid number */
			lu_ssc->allow_overwrite_block = 0;
			lu_ssc->allow_overwrite_block--;
			mkSenseBuf(ILLEGAL_REQUEST,
					E_SEQUENTIAL_POSITIONING_ERROR,
					sam_stat);
			ret_stat = SAM_STAT_CHECK_CONDITION;
		}
		break;
	case 2:
		lu_ssc->allow_overwrite = 2;
		break;
	default:
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB, sam_stat);
		ret_stat = SAM_STAT_CHECK_CONDITION;
		break;
	}

	return ret_stat;
}

uint8_t ssc_log_select(struct scsi_cmd *cmd)
{
	uint8_t sam_status;
	uint8_t pcr = cmd->scb[1] & 0x2;	/* Parameter code reset */
	struct priv_lu_ssc *lu_priv;

	lu_priv = cmd->lu->lu_private;

	sam_status = spc_log_select(cmd);
	if (sam_status)	/* spc_log_select() failed - return */
		return sam_status;

	if (pcr) {
		switch ((cmd->scb[2] & 0xc0) >> 6) {
		case 3:
			lu_priv->bytesRead_I = 0;
			lu_priv->bytesRead_M = 0;
			lu_priv->bytesWritten_I = 0;
			lu_priv->bytesWritten_M = 0;
			break;
		}
	}
	return SAM_STAT_GOOD;
}

uint8_t ssc_read_6(struct scsi_cmd *cmd)
{
	uint8_t *cdb = cmd->scb;
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;
	struct vtl_ds *dbuf_p;
	struct priv_lu_ssc *lu_ssc;
	uint8_t *buf;
	int count;
	int sz;
	int k;
	int retval = 0;
	int fixed;

	lu_ssc = cmd->lu->lu_private;
	dbuf_p = cmd->dbuf_p;

	current_state = MHVTL_STATE_READING;

	fixed = cdb[1] & FIXED;	/* Fixed block read ? */
	if (fixed) {
		count = get_unaligned_be24(&cdb[2]);
		sz = get_unaligned_be24(&modeBlockDescriptor[5]);
		MHVTL_DBG(last_cmd == READ_6 ? 2 : 1,
			"READ_6 (%ld) : \"Fixed block read\" "
			" %d blocks of %d size",
					(long)dbuf_p->serialNo, count, sz);
	} else { /* else - Variable block read */
		sz = get_unaligned_be24(&cdb[2]);
		count = 1;
		MHVTL_DBG(last_cmd == READ_6 ? 2 : 1,
				"READ_6 (%ld) : %d bytes **",
					(long)dbuf_p->serialNo, sz);
	}

	/* If both FIXED & SILI bits set, invalid combo.. */
	if ((cdb[1] & (SILI | FIXED)) == (SILI | FIXED)) {
		MHVTL_DBG(1, "Suppress ILI and Fixed block "
					"read not allowed by SSC3");
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	switch (lu_ssc->tapeLoaded) {
	case TAPE_LOADED:
		if (mam.MediumType == MEDIA_TYPE_CLEAN) {
			MHVTL_DBG(3, "Cleaning cart loaded");
			mkSenseBuf(NOT_READY, E_CLEANING_CART_INSTALLED,
								sam_stat);
			return SAM_STAT_CHECK_CONDITION;
		}
		break;
	case TAPE_UNLOADED:
		MHVTL_DBG(3, "No media loaded");
		mkSenseBuf(NOT_READY, E_MEDIUM_NOT_PRESENT, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
		break;
	default:
		MHVTL_DBG(1, "Media format corrupt");
		mkSenseBuf(NOT_READY, E_MEDIUM_FMT_CORRUPT, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
		break;
	}

	buf = dbuf_p->data;
	for (k = 0; k < count; k++) {
		if (!lu_ssc->pm->valid_encryption_blk(cmd))
			return SAM_STAT_CHECK_CONDITION;
		retval = readBlock(buf, sz, cdb[1] & SILI, sam_stat);
		if (!retval && fixed) {
			/* Fixed block read hack:
			 * Overwrite INFORMATION field with:
			 * The INFORMATION field shall be set to the requested
			 * transfer length minus the actual number of logical
			 * blocks read (not including the incorrect-length
			 * logical block).
			 */
			put_unaligned_be32(count - k, &sense[3]);
			break;
		}
		buf += retval;
		dbuf_p->sz += retval;
	}
	if (retval > (sz * count))
		retval = sz * count;

	return *sam_stat;
}

uint8_t ssc_write_6(struct scsi_cmd *cmd)
{
	uint8_t *cdb = cmd->scb;
	struct vtl_ds *dbuf_p;
	struct priv_lu_ssc *lu_ssc;
	int count;
	int sz;
	int k;
	int retval = 0;

	lu_ssc = cmd->lu->lu_private;
	dbuf_p = cmd->dbuf_p;

	current_state = MHVTL_STATE_WRITING;

	if (cdb[1] & FIXED) {	/* If Fixed block writes */
		count = get_unaligned_be24(&cdb[2]);
		sz = get_unaligned_be24(&modeBlockDescriptor[5]);
		MHVTL_DBG(last_cmd == WRITE_6 ? 2 : 1,
				"WRITE_6: %d blks of %d bytes (%ld) **",
						count,
						sz,
						(long)dbuf_p->serialNo);
	} else {		 /* else - Variable Block writes */
		count = 1;
		sz = get_unaligned_be24(&cdb[2]);
		MHVTL_DBG(last_cmd == WRITE_6 ? 2 : 1,
				"WRITE_6: %d bytes (%ld) **",
						sz,
						(long)dbuf_p->serialNo);
	}

	/* FIXME: Should handle this instead of 'check & warn' */
	if ((sz * count) > lu_ssc->bufsize)
		MHVTL_DBG(1,
			"Fatal: bufsize %d, requested write of %d bytes",
							lu_ssc->bufsize, sz);

	/* Retrieve data from kernel */
	dbuf_p->sz = sz * count;
	retrieve_CDB_data(cmd->cdev, dbuf_p);

	if (!lu_ssc->pm->check_restrictions(cmd))
		return SAM_STAT_CHECK_CONDITION;

	if (OK_to_write) {
		for (k = 0; k < count; k++) {
			retval = writeBlock(cmd, sz);
			dbuf_p->data += retval;

			/* If sam_stat != SAM_STAT_GOOD, return */
			if (cmd->dbuf_p->sam_stat)
				return cmd->dbuf_p->sam_stat;
		}
	}
	return SAM_STAT_GOOD;
}

/*
 * Check for any write restrictions - e.g. WORM, or Clean Cartridge mounted.
 * Return 1 = OK to write, zero -> Can't write.
 */
uint8_t check_restrictions(struct scsi_cmd *cmd)
{
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;
	struct priv_lu_ssc *lu_ssc = cmd->lu->lu_private;

	/* Check that there is a piece of media loaded.. */
	switch (lu_ssc->tapeLoaded) {
	case TAPE_LOADED:	/* Do nothing */
		break;
	case TAPE_UNLOADED:
		mkSenseBuf(NOT_READY, E_MEDIUM_NOT_PRESENT, sam_stat);
		*lu_ssc->OK_2_write = 0;
		return *lu_ssc->OK_2_write;
		break;
	default:
		mkSenseBuf(NOT_READY, E_MEDIUM_FMT_CORRUPT, sam_stat);
		*lu_ssc->OK_2_write = 0;
		return *lu_ssc->OK_2_write;
		break;
	}

	switch (mam.MediumType) {
	case MEDIA_TYPE_CLEAN:
		mkSenseBuf(NOT_READY, E_CLEANING_CART_INSTALLED, sam_stat);
		MHVTL_DBG(2, "Can not write - Cleaning cart");
		*lu_ssc->OK_2_write = 0;
		break;
	case MEDIA_TYPE_WORM:
		/* If we are not at end of data for a write
		 * and media is defined as WORM, fail...
		 */
		if (c_pos->blk_type == B_EOD)
			 /* OK to append to end of 'tape' */
			*lu_ssc->OK_2_write = 1;

		if (!*lu_ssc->OK_2_write) {
			MHVTL_DBG(1, "Failed attempt to overwrite WORM data");
			mkSenseBuf(DATA_PROTECT,
				E_MEDIUM_OVERWRITE_ATTEMPTED, sam_stat);
		}
		break;
	case MEDIA_TYPE_DATA:
		*lu_ssc->OK_2_write = 1;
		break;
	default:
		*lu_ssc->OK_2_write = 0;
		mkSenseBuf(ILLEGAL_REQUEST, E_MEDIUM_INCOMPATIBLE, sam_stat);
	}

	/* over-ride the above IF the virtual write protect switch is on */
	if (*lu_ssc->OK_2_write && lu_ssc->MediaWriteProtect) {
		*lu_ssc->OK_2_write = 0;
		mkSenseBuf(DATA_PROTECT, E_WRITE_PROTECT, sam_stat);
	}

	/* over-ride the above IF running in append_only mode and this write
	 * isn't authorized.
	 * Some writes would be OK, like the first write to an empty tape or
	 * WORM media overwriting a filemark that is next to EOD
	 */
	if (lu_ssc->OK_2_write && lu_ssc->append_only_mode) {
		if ((c_pos->blk_number != lu_ssc->allow_overwrite_block) &&
				(c_pos->blk_type != B_EOD)) {

			uint64_t TAflag;

			lu_ssc->OK_2_write = 0;
			lu_ssc->allow_overwrite = FALSE;
			mkSenseBuf(DATA_PROTECT, E_MEDIUM_OVERWRITE_ATTEMPTED,
						sam_stat);
			/* And set TapeAlert flg 09 -> WRITE PROTECT */
			TAflag = 0x100;
			update_TapeAlert(cmd->lu, TAflag);
		}
	}

	MHVTL_DBG(2, "returning %s",
			(*lu_ssc->OK_2_write) ? "Writable" : "Non-writable");
	return *lu_ssc->OK_2_write;
}

/*
 * Returns true if blk header has correct encryption key data
 */
#define	UKAD_LENGTH	encr->ukad_length
#define	AKAD_LENGTH	encr->akad_length
#define	KEY_LENGTH	encr->key_length
#define	UKAD		encr->ukad
#define	AKAD		encr->akad
#define	KEY		encr->key
uint8_t valid_encryption_blk(struct scsi_cmd *cmd)
{
	uint8_t correct_key;
	int i;
	struct lu_phy_attr *lu = cmd->lu;
	struct priv_lu_ssc *lu_priv;
	struct encryption *encr;
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;

	lu_priv = lu->lu_private;
	encr = lu_priv->encr;

	/* decryption logic */
	correct_key = TRUE;
	if (c_pos->blk_flags & BLKHDR_FLG_ENCRYPTED) {
		/* compare the keys */
		if (lu_priv->DECRYPT_MODE > 1) {
			if (c_pos->encryption.key_length != KEY_LENGTH) {
				mkSenseBuf(DATA_PROTECT, E_INCORRECT_KEY, sam_stat);
				correct_key = FALSE;
			}
			for (i = 0; i < c_pos->encryption.key_length; ++i) {
				if (c_pos->encryption.key[i] != KEY[i]) {
					mkSenseBuf(DATA_PROTECT,
							E_INCORRECT_KEY,
							sam_stat);
					correct_key = FALSE;
					return correct_key;
				}
			}
		} else {
			mkSenseBuf(DATA_PROTECT, E_UNABLE_TO_DECRYPT, sam_stat);
			correct_key = FALSE;
		}
	} else if (lu_priv->DECRYPT_MODE == 2) {
		mkSenseBuf(DATA_PROTECT, E_UNENCRYPTED_DATA, sam_stat);
		correct_key = FALSE;
	}
	return correct_key;
}

uint8_t valid_encryption_media(struct scsi_cmd *cmd)
{
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;
	struct lu_phy_attr *lu;
	struct priv_lu_ssc *lu_priv;

	lu = cmd->lu;
	lu_priv = lu->lu_private;

	if (c_pos->blk_number == 0) {
		modeBlockDescriptor[0] = lu_priv->pm->native_drive_density->density;
		mam.MediumDensityCode = modeBlockDescriptor[0];
		mam.FormattedDensityCode = modeBlockDescriptor[0];
		rewriteMAM(sam_stat);
	} else {
		if (mam.MediumDensityCode !=
				lu_priv->pm->native_drive_density->density) {
			mkSenseBuf(DATA_PROTECT, E_WRITE_PROTECT, sam_stat);
			return SAM_STAT_CHECK_CONDITION;
		}
	}

	return SAM_STAT_GOOD;
}

uint8_t ssc_allow_prevent_removal(struct scsi_cmd *cmd)
{
	/* FIXME: Currently does nothing... */
	MHVTL_DBG(1, "%s MEDIA removal (%ld) **",
					(cmd->scb[4]) ? "Prevent" : "Allow",
					(long)cmd->dbuf_p->serialNo);
	return SAM_STAT_GOOD;
}

uint8_t ssc_format_media(struct scsi_cmd *cmd)
{
	struct lu_phy_attr *lu;
	struct priv_lu_ssc *lu_priv;

	lu = cmd->lu;
	lu_priv = lu->lu_private;

	MHVTL_DBG(1, "Format Medium (%ld) **", (long)cmd->dbuf_p->serialNo);

	if (!lu_priv->pm->check_restrictions(cmd))
		return SAM_STAT_CHECK_CONDITION;

	if (c_pos->blk_number != 0) {
		MHVTL_DBG(2, "Not at beginning **");
		mkSenseBuf(ILLEGAL_REQUEST, E_POSITION_PAST_BOM,
					&cmd->dbuf_p->sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}
	format_tape(&cmd->dbuf_p->sam_stat);

	return SAM_STAT_GOOD;
}

uint8_t ssc_seek_10(struct scsi_cmd *cmd)
{
	uint32_t blk_no;

	current_state = MHVTL_STATE_LOCATE;

	MHVTL_DBG(1, "Fast Block Locate (%ld) **",
						(long)cmd->dbuf_p->serialNo);
	blk_no = get_unaligned_be32(&cmd->scb[3]);

	/* If we want to seek closer to beginning of file than
	 * we currently are, rewind and seek from there
	 */
	MHVTL_DBG(2, "Current blk: %d, seek: %d",
					c_pos->blk_number, blk_no);
	position_to_block(blk_no, &cmd->dbuf_p->sam_stat);

	return cmd->dbuf_p->sam_stat;
}

uint8_t ssc_load_display(struct scsi_cmd *cmd)
{
	unsigned char *d;
	char str1[9];
	char str2[9];

	MHVTL_DBG(1, "LOAD DISPLAY (%ld) - T10000 specific **",
					(long)cmd->dbuf_p->serialNo);

	cmd->dbuf_p->sz = cmd->scb[4];
	retrieve_CDB_data(cmd->cdev, cmd->dbuf_p);
	d = cmd->dbuf_p->data;
	memcpy(str1, &d[1], 8);
	str1[8] = 0;
	memcpy(str2, &d[9], 8);
	str2[8] = 0;

	MHVTL_DBG(3, "Raw data: %02x  "
			"%02x %02x %02x %02x %02x %02x %02x %02x  "
			"%02x %02x %02x %02x %02x %02x %02x %02x",
			d[0], d[1], d[2], d[3], d[4],
			d[5], d[6], d[7], d[8],
			d[9], d[10], d[11], d[12],
			d[13], d[14], d[15], d[16]);

	switch (d[0] >> 5) { /* Bits 5, 6 & 7 are overlay */
	case 0:
		MHVTL_DBG(1, "Display \'%s\' until next"
				" command that initiates tape motion",
	/* Low/High bit */	(d[0] & 2) ? str2 : str1);
		break;
	case 1:
		MHVTL_DBG(1, "Maintain \'%s\' until the"
				" cartridge is unloaded",
	/* Low/High bit */	(d[0] & 2) ? str2 : str1);
		break;
	case 2:
		MHVTL_DBG(1, "Maintain \'%s\' until the drive"
				" is next loaded", str1);
		break;
	case 3:
		MHVTL_DBG(1, "Physically access tape drive with"
				"out changing the msg");
		break;
	case 7:
		MHVTL_DBG(1, "Display \'%s\' until the tape"
				" drive is unloaded then \'%s\'",
					str1, str2);
		break;
	}
	MHVTL_DBG(2, "Load display: msg1: %s msg2: %s",
					str1, str2);

	cmd->dbuf_p->sz = 0;
	return SAM_STAT_GOOD;
}

uint8_t ssc_a3_service_action(struct scsi_cmd *cmd)
{
	switch (cmd->scb[1]) {
	case MANAGEMENT_PROTOCOL_IN:
		log_opcode("MANAGEMENT PROTOCOL IN **", cmd);
		break;
	case REPORT_ALIASES:
		log_opcode("REPORT ALIASES **", cmd);
		break;
	}
	log_opcode("Unknown service action A3 **", cmd);
	return cmd->dbuf_p->sam_stat;
}

uint8_t ssc_a4_service_action(struct scsi_cmd *cmd)
{
	switch (cmd->scb[1]) {
	case MANAGEMENT_PROTOCOL_OUT:
		log_opcode("MANAGEMENT PROTOCOL OUT **", cmd);
		break;
	case CHANGE_ALIASES:
		log_opcode("CHANGE ALIASES **", cmd);
		break;
	case FORCED_EJECT:
		log_opcode("FORCED EJECT **", cmd);
		break;
	}
	log_opcode("Unknown service action A4 **", cmd);
	return cmd->dbuf_p->sam_stat;
}

uint8_t ssc_spout(struct scsi_cmd *cmd)
{
	MHVTL_DBG(1, "Security Protocol Out (%ld) **",
						(long)cmd->dbuf_p->serialNo);

	cmd->dbuf_p->sz = get_unaligned_be32(&cmd->scb[6]);
	/* Check for '512 increment' bit & multiply sz by 512 if set */
	cmd->dbuf_p->sz *= (cmd->scb[4] & 0x80) ? 512 : 1;

	retrieve_CDB_data(cmd->cdev, cmd->dbuf_p);

	return resp_spout(cmd);
}

uint8_t ssc_spin(struct scsi_cmd *cmd)
{
	MHVTL_DBG(1, "Security Protocol In (%ld) **",
					(long)cmd->dbuf_p->serialNo);

	return resp_spin(cmd);
}

uint8_t ssc_pr_out(struct scsi_cmd *cmd)
{
	struct priv_lu_ssc *lu_priv;
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;

	lu_priv = cmd->lu->lu_private;

	MHVTL_DBG(1, "PERSISTENT RESERVE OUT (%ld) **",
						(long)cmd->dbuf_p->serialNo);
	if (lu_priv->I_am_SPC_2_Reserved) {
		MHVTL_DBG(1, "SPC 2 reserved");
		*sam_stat = SAM_STAT_RESERVATION_CONFLICT;
		return SAM_STAT_RESERVATION_CONFLICT;
	}
	cmd->dbuf_p->sz = get_unaligned_be32(&cmd->scb[5]);
	retrieve_CDB_data(cmd->cdev, cmd->dbuf_p);
	return resp_spc_pro(cmd->scb, cmd->dbuf_p);
}

static uint8_t set_device_configuration_extension(struct scsi_cmd *cmd, uint8_t *p)
{
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;
	struct lu_phy_attr *lu = cmd->lu;
	struct priv_lu_ssc *lu_priv = cmd->lu->lu_private;
	struct ssc_personality_template *pm;
	struct mode *mp;
	int page_code_len;
	int write_mode;
	int pews;	/* Programable Early Warning Size */

	pm = lu_priv->pm;

	mp = lookup_pcode(&lu->mode_pg, MODE_DEVICE_CONFIGURATION, 1);

	/* Code error
	 * Any device supporting this should have this mode page defined */
	if (!mp) {
		mkSenseBuf(HARDWARE_ERROR, E_INTERNAL_TARGET_FAILURE, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	page_code_len = get_unaligned_be16(&p[2]);

	if (page_code_len != 0x1c) {
		MHVTL_LOG("Unexpected page code length.. Unexpected results");
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_PARMS, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	write_mode = (p[5] & 0xf0) >> 4;
	if (write_mode > 1) {
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_PARMS, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}
	MHVTL_DBG(2, "%s mode", write_mode ? "Append-only" : "Write-anywhere");

	pews = get_unaligned_be16(&p[6]);
	if (pm->drive_supports_prog_early_warning) {
		MHVTL_DBG(2, "Set Programable Early Warning Size: %d", pews);
		lu_priv->prog_early_warning_sz = pews;
		update_prog_early_warning(lu);
	} else {
		MHVTL_DBG(2, "Programable Early Warning Size not supported"
				" by this device");
	}

	MHVTL_DBG(2, "Volume containing encrypted logical blocks "
			"requires encryption: %d",
			p[8] & 0x01);

	if (pm->drive_supports_append_only_mode) {
		/* Can't reset append-only mode via mode page ssc4 8.3.8 */
		if (lu_priv->append_only_mode && write_mode == 0) {
			MHVTL_LOG("Can't reset append only mode via mode page");
			mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_PARMS,
							sam_stat);
			return SAM_STAT_CHECK_CONDITION;
		}
		if (write_mode) {
			lu_priv->append_only_mode = write_mode;
			lu_priv->allow_overwrite = FALSE;
		}
	}

	/* Now update our copy of this mode page */
	mp->pcodePointer[5] &= 0x0f;
	mp->pcodePointer[5] |= write_mode << 4;

	return SAM_STAT_GOOD;
}

static void set_device_configuration(struct scsi_cmd *cmd, uint8_t *p)
{
	struct lu_phy_attr *lu = cmd->lu;
	struct priv_lu_ssc *lu_priv = cmd->lu->lu_private;
	struct ssc_personality_template *pm;

	pm = lu_priv->pm;

	MHVTL_DBG(2, " Report Early Warning   : %s",
			(p[8] & 0x01) ? "Yes" : "No");
	MHVTL_DBG(2, " Software Write Protect : %s",
			(p[10] & 0x04) ? "Yes" : "No");
	MHVTL_DBG(2, " WORM Tamper Read Enable: %s",
			(p[15] & 0x80) ? "Yes" : "No");

	MHVTL_DBG(2, " Setting device compression Algorithm");
	if (p[14]) { /* Select Data Compression Alg */
		MHVTL_DBG(2, "  Mode Select->Setting compression: %d", p[14]);
		if (pm->set_compression)
			pm->set_compression(&lu->mode_pg,
				lu_priv->configCompressionFactor);
	} else {
		MHVTL_DBG(2, "  Mode Select->Clearing compression");
		if (pm->clear_compression)
			pm->clear_compression(&lu->mode_pg);
	}
}

static void set_mode_compression(struct scsi_cmd *cmd, uint8_t *p)
{
	struct lu_phy_attr *lu = cmd->lu;
	struct priv_lu_ssc *lu_priv = lu->lu_private;
	struct ssc_personality_template *pm;
	int dce;

	pm = lu_priv->pm;
	dce = p[2] & 0x80;

	MHVTL_DBG(2, " Data Compression Enable   : %s (0x%02x)",
				(p[1] & 0x80) ? "Yes" : "No", p[1]);
	MHVTL_DBG(2, " Data Compression Capable  : %s",
				(p[1] & 0x40) ? "Yes" : "No");
	MHVTL_DBG(2, " Data DeCompression Enable : %s (0x%02x)",
				(p[2] & 0x80) ? "Yes" : "No", p[2]);
	MHVTL_DBG(2, " Compression Algorithm     : 0x%04x",
				get_unaligned_be32(&p[4]));
	MHVTL_DBG(2, " DeCompression Algorithm   : 0x%04x",
				get_unaligned_be32(&p[8]));
	MHVTL_DBG(2, " Report Exception on Decompression: 0x%02x",
				(p[3] & 0x6) >> 5);

	if (dce) { /* Data Compression Enable bit set */
		MHVTL_DBG(1, " Setting compression");
		if (pm->set_compression)
			pm->set_compression(&lu->mode_pg,
					lu_priv->configCompressionFactor);
	} else {
		MHVTL_DBG(1, " Clearing compression");
		if (pm->clear_compression)
			pm->clear_compression(&lu->mode_pg);
	}
}

/*
 * Process the MODE_SELECT command
 */
uint8_t ssc_mode_select(struct scsi_cmd *cmd)
{
	uint8_t *sam_stat = &cmd->dbuf_p->sam_stat;
	uint8_t *buf = cmd->dbuf_p->data;
	int block_descriptor_sz;
	int page_len;
	uint8_t *bdb = NULL;
	int i;
	int long_lba = 0;
	int count;
	int save_page;

	save_page = cmd->scb[1] & 0x01;

	switch (cmd->scb[0]) {
	case MODE_SELECT:
		cmd->dbuf_p->sz = cmd->scb[4];
		break;
	case MODE_SELECT_10:
		cmd->dbuf_p->sz = get_unaligned_be16(&cmd->scb[7]);
		break;
	default:
		cmd->dbuf_p->sz = 0;
	}

	count = retrieve_CDB_data(cmd->cdev, cmd->dbuf_p);

	MHVTL_DBG(1, "MODE SELECT (%ld) **", (long)cmd->dbuf_p->serialNo);

	if (!(cmd->scb[1] & 0x10)) { /* Page Format: 1 - SPC, 0 - vendor uniq */
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	switch (cmd->scb[0]) {
	case MODE_SELECT:
		block_descriptor_sz = buf[3];
		if (block_descriptor_sz)
			bdb = &buf[4];
		i = 4 + block_descriptor_sz;
		break;
	case MODE_SELECT_10:
		block_descriptor_sz = get_unaligned_be16(&buf[6]);
		long_lba = buf[4] & 1;
		if (block_descriptor_sz)
			bdb = &buf[8];
		i = 8 + block_descriptor_sz;
		break;
	default:
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_OP_CODE, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	if (bdb) {
		if (long_lba) {
			mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB,
							sam_stat);
			MHVTL_DBG(1, "Warning can not "
				"handle long descriptor block (long_lba bit)");
			return SAM_STAT_CHECK_CONDITION;
		}
		memcpy(modeBlockDescriptor, bdb, block_descriptor_sz);
	}

	/* Ignore mode pages if 'save page' bit not set */
	if (!save_page) {
		MHVTL_DBG(1, "Save page bit not set. Ignoring page data");
		return SAM_STAT_GOOD;
	}

#ifdef MHVTL_DEBUG
	if (debug)
		hex_dump(buf, cmd->dbuf_p->sz);
#endif

	MHVTL_DBG(3, "count: %d, i: %d", count, i);
	if (i == 4) {
	MHVTL_DBG(3, "Offset 0: %02x %02x %02x %02x",
			buf[0], buf[1], buf[2], buf[3]);
	} else {
	MHVTL_DBG(3, "Offset 0: %02x %02x %02x %02x  %02x %02x %02x %02x",
			buf[0], buf[1], buf[2], buf[3],
			buf[4], buf[5], buf[6], buf[7]);
	}

	count -= i;
	while (i < count) {
		MHVTL_DBG(3, " %02d: %02x %02x %02x %02x  %02x %02x %02x %02x",
			i,
			buf[i+0], buf[i+1], buf[i+2], buf[i+3],
			buf[i+4], buf[i+5], buf[i+6], buf[i+7]);
		MHVTL_DBG(3, " %02d: %02x %02x %02x %02x  %02x %02x %02x %02x",
			i+8,
			buf[i+8], buf[i+9], buf[i+10], buf[i+11],
			buf[i+12], buf[i+13], buf[i+14], buf[i+15]);
		MHVTL_DBG(3, " %02d: %02x %02x %02x %02x  %02x %02x %02x %02x",
			i+16,
			buf[i+16], buf[i+17], buf[i+18], buf[i+19],
			buf[i+20], buf[i+21], buf[i+22], buf[i+23]);
		MHVTL_DBG(3, " %02d: %02x %02x %02x %02x  %02x %02x %02x %02x",
			i+24,
			buf[i+24], buf[i+25], buf[i+26], buf[i+27],
			buf[i+28], buf[i+29], buf[i+30], buf[i+31]);
		/* Default page len is, override if sub-pages */
		page_len = buf[i + 1];
		switch (buf[i]) {
		case MODE_DATA_COMPRESSION:
			set_mode_compression(cmd, &buf[i]);
			break;

		case MODE_DEVICE_CONFIGURATION:
			/* If this is '01' it's a subpage value
			 *     i.e. DEVICE CONFIGURATION EXTENSION
			 * If it's 0x0e, it indicates a page length
			 * for MODE DEVICE CONFIGURATION
			 */
			if (buf[i + 1] == 0x01) {
				if (set_device_configuration_extension(cmd,
								&buf[i]))
					return SAM_STAT_CHECK_CONDITION;
				/* Subpage 1 - override default page length */
				page_len = get_unaligned_be16(&buf[i + 2]);
			} else
				set_device_configuration(cmd, &buf[i]);
			break;

		default:
			MHVTL_DBG_PRT_CDB(1, cmd);
			MHVTL_DBG(1, "Mode page 0x%02x not handled", buf[i]);
			break;
		}
		if (page_len == 0) { /* Something wrong with data structure */
			page_len = cmd->dbuf_p->sz;
			MHVTL_LOG("Problem with mode select data structure");
		}
		i += page_len;	/* Next mode page */
	}

	return SAM_STAT_GOOD;
}

uint8_t ssc_write_attributes(struct scsi_cmd *cmd)
{
	int sz;
	struct priv_lu_ssc *lu_priv;
	uint8_t *sam_stat;

	lu_priv = cmd->lu->lu_private;
	sam_stat = &cmd->dbuf_p->sam_stat;

	MHVTL_DBG(1, "Write Attributes (%ld) **", (long)cmd->dbuf_p->serialNo);

	switch (lu_priv->tapeLoaded) {
	case TAPE_UNLOADED:
		mkSenseBuf(NOT_READY, E_MEDIUM_NOT_PRESENT, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
		break;
	case TAPE_LOADED:
		cmd->dbuf_p->sz = get_unaligned_be32(&cmd->scb[10]);
		sz = retrieve_CDB_data(cmd->cdev, cmd->dbuf_p);
		MHVTL_DBG(1, "  --> Expected to read %d bytes"
				", read %d", cmd->dbuf_p->sz, sz);
		if (resp_write_attribute(cmd) > 0)
			rewriteMAM(sam_stat);
		break;
	default:
		mkSenseBuf(NOT_READY, E_MEDIUM_FMT_CORRUPT, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
		break;
	}
	return SAM_STAT_GOOD;
}

uint8_t ssc_tur(struct scsi_cmd *cmd)
{
	struct priv_lu_ssc *lu_priv;
	uint8_t *sam_stat;
	char str[64];

	lu_priv = cmd->lu->lu_private;
	sam_stat = &cmd->dbuf_p->sam_stat;
	*sam_stat = SAM_STAT_GOOD;

	sprintf(str, "Test Unit Ready (%ld) ** : ",
				(long)cmd->dbuf_p->serialNo);

	switch (lu_priv->tapeLoaded) {
	case TAPE_UNLOADED:
		strcat(str, "No, No tape loaded");
		mkSenseBuf(NOT_READY, E_MEDIUM_NOT_PRESENT, sam_stat);
		*sam_stat = SAM_STAT_CHECK_CONDITION;
		break;
	case TAPE_LOADED:
		if (mam.MediumType == MEDIA_TYPE_CLEAN) {
			int state;

			strcat(str, "No, Cleaning cart loaded");

			if (lu_priv->cleaning_media_state)
				state = *lu_priv->cleaning_media_state;
			else
				state = 0;

			switch (state) {
			case CLEAN_MOUNT_STAGE1:
				mkSenseBuf(NOT_READY, E_CLEANING_CART_INSTALLED,
								sam_stat);
				break;
			case CLEAN_MOUNT_STAGE2:
				mkSenseBuf(NOT_READY, E_CAUSE_NOT_REPORTABLE,
								sam_stat);
				break;
			case CLEAN_MOUNT_STAGE3:
				mkSenseBuf(NOT_READY, E_INITIALIZING_REQUIRED,
								sam_stat);
				break;
			default:
				MHVTL_ERR("Unknown cleaning media mount state");
				mkSenseBuf(NOT_READY, E_CLEANING_CART_INSTALLED,
								sam_stat);
				break;
			}

			*sam_stat = SAM_STAT_CHECK_CONDITION;
		} else
			strcat(str, "Yes");
		break;
	default:
		strcat(str, "No, Media format corrupt");
		mkSenseBuf(NOT_READY, E_MEDIUM_FMT_CORRUPT, sam_stat);
		*sam_stat = SAM_STAT_CHECK_CONDITION;
		break;
	}

	MHVTL_DBG(1, "%s", str);

	return *sam_stat;
}

uint8_t ssc_rewind(struct scsi_cmd *cmd)
{
	struct priv_lu_ssc *lu_priv;
	uint8_t *sam_stat;
	int retval;

	lu_priv = cmd->lu->lu_private;
	sam_stat = &cmd->dbuf_p->sam_stat;

	MHVTL_DBG(1, "Rewinding (%ld) **", (long)cmd->dbuf_p->serialNo);

	current_state = MHVTL_STATE_REWIND;

	switch (lu_priv->tapeLoaded) {
	case TAPE_UNLOADED:
		mkSenseBuf(NOT_READY, E_MEDIUM_NOT_PRESENT, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
		break;
	case TAPE_LOADED:
		retval = rewind_tape(sam_stat);
		if (retval < 0) {
			mkSenseBuf(NOT_READY, E_MEDIUM_FMT_CORRUPT, sam_stat);
			return SAM_STAT_CHECK_CONDITION;
		}
		break;
	default:
		mkSenseBuf(NOT_READY, E_MEDIUM_FMT_CORRUPT, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
		break;
	}

	return SAM_STAT_GOOD;
}

uint8_t ssc_read_attributes(struct scsi_cmd *cmd)
{
	struct priv_lu_ssc *lu_priv;
	uint8_t *sam_stat;

	lu_priv = cmd->lu->lu_private;
	sam_stat = &cmd->dbuf_p->sam_stat;

	MHVTL_DBG(1, "Read Attribute (%ld) **",
						(long)cmd->dbuf_p->serialNo);

	switch (lu_priv->tapeLoaded) {
	case TAPE_UNLOADED:
		MHVTL_DBG(1, "Failed due to \"no media loaded\"");
		mkSenseBuf(NOT_READY, E_MEDIUM_NOT_PRESENT, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
		break;
	case TAPE_LOADED:
		break;
	default:
		MHVTL_DBG(1, "Failed due to \"media corrupt\"");
		mkSenseBuf(NOT_READY, E_MEDIUM_FMT_CORRUPT, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
		break;
	}

	/* Only support Service Action - Attribute Values */
	if (cmd->scb[1] > 1) {
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	cmd->dbuf_p->sz = resp_read_attribute(cmd);

	return SAM_STAT_GOOD;
}

uint8_t ssc_read_block_limits(struct scsi_cmd *cmd)
{
	struct priv_lu_ssc *lu_priv;
	uint8_t *sam_stat;

	lu_priv = cmd->lu->lu_private;
	sam_stat = &cmd->dbuf_p->sam_stat;

	MHVTL_DBG(1, "Read block limits (%ld) **",
						(long)cmd->dbuf_p->serialNo);

	switch (lu_priv->tapeLoaded) {
	case TAPE_LOADED:
	case TAPE_UNLOADED:
		cmd->dbuf_p->sz = resp_read_block_limits(cmd->dbuf_p,
							lu_priv->bufsize);
		break;
	default:
		mkSenseBuf(NOT_READY, E_MEDIUM_FMT_CORRUPT, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
		break;
	}

	return SAM_STAT_GOOD;
}

uint8_t ssc_read_media_sn(struct scsi_cmd *cmd)
{
	struct priv_lu_ssc *lu_priv;
	uint8_t *sam_stat;

	lu_priv = cmd->lu->lu_private;
	sam_stat = &cmd->dbuf_p->sam_stat;

	MHVTL_DBG(1, "Read Medium Serial No. (%ld) **",
						(long)cmd->dbuf_p->serialNo);
	switch (lu_priv->tapeLoaded) {
	case TAPE_LOADED:
		cmd->dbuf_p->sz = resp_read_media_serial(lu_priv->mediaSerialNo,
							cmd->dbuf_p->data,
							sam_stat);
		break;
	case TAPE_UNLOADED:
		mkSenseBuf(NOT_READY, E_MEDIUM_NOT_PRESENT, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
		break;
	default:
		mkSenseBuf(NOT_READY, E_MEDIUM_FMT_CORRUPT, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
		break;
	}
	return *sam_stat;
}

uint8_t ssc_read_position(struct scsi_cmd *cmd)
{
	struct priv_lu_ssc *lu_priv;
	uint8_t *sam_stat;
	int service_action;

	lu_priv = cmd->lu->lu_private;
	sam_stat = &cmd->dbuf_p->sam_stat;

	MHVTL_DBG(1, "Read Position (%ld) **", (long)cmd->dbuf_p->serialNo);

	service_action = cmd->scb[1] & 0x1f;
	/* service_action == 0 or 1 -> Returns 20 bytes of data (short) */

	*sam_stat = SAM_STAT_GOOD;

	switch (lu_priv->tapeLoaded) {
	case TAPE_LOADED:
		if ((service_action == 0) || (service_action == 1))
			cmd->dbuf_p->sz = resp_read_position(c_pos->blk_number,
							cmd->dbuf_p->data,
							sam_stat);
		break;
	case TAPE_UNLOADED:
		mkSenseBuf(NOT_READY, E_MEDIUM_NOT_PRESENT, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
		break;
	default:
		mkSenseBuf(NOT_READY, E_MEDIUM_FMT_CORRUPT, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
		break;
	}
	return *sam_stat;
}

uint8_t ssc_release(struct scsi_cmd *cmd)
{
	struct priv_lu_ssc *lu_priv;

	lu_priv = cmd->lu->lu_private;

	MHVTL_DBG(1, "Release (%ld) **", (long)cmd->dbuf_p->serialNo);
	if (!SPR_Reservation_Type && SPR_Reservation_Key)
		return SAM_STAT_RESERVATION_CONFLICT;

	lu_priv->I_am_SPC_2_Reserved = 0;
	return SAM_STAT_GOOD;
}

uint8_t ssc_report_density_support(struct scsi_cmd *cmd)
{
	struct priv_lu_ssc *lu_priv;
	uint8_t *sam_stat;
	uint8_t media;

	lu_priv = cmd->lu->lu_private;
	sam_stat = &cmd->dbuf_p->sam_stat;

	media = cmd->scb[1] & 0x01;
	cmd->dbuf_p->sz = 0;

	MHVTL_DBG(1, "Report %s Density Support (%ld) **",
					(media) ? "mounted Media" : "Drive",
					(long)cmd->dbuf_p->serialNo);

	if (cmd->scb[1] & 0x02) { /* Don't support Medium Type (yet) */
		MHVTL_DBG(1, "Medium Type - not currently supported");
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	if (media == 1 && lu_priv->tapeLoaded != TAPE_LOADED) {
		MHVTL_DBG(1, "Media has to be mounted to return media density");
		mkSenseBuf(NOT_READY, E_MEDIUM_NOT_PRESENT, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	cmd->dbuf_p->sz = get_unaligned_be16(&cmd->scb[7]);
	cmd->dbuf_p->sz = resp_report_density(lu_priv, media, cmd->dbuf_p);
	return SAM_STAT_GOOD;
}

uint8_t ssc_reserve(struct scsi_cmd *cmd)
{
	struct priv_lu_ssc *lu_priv;

	lu_priv = cmd->lu->lu_private;

	MHVTL_DBG(1, "Reserve (%ld) **", (long)cmd->dbuf_p->serialNo);
	if (!SPR_Reservation_Type && !SPR_Reservation_Key)
		lu_priv->I_am_SPC_2_Reserved = 1;
	if (!SPR_Reservation_Type && SPR_Reservation_Key)
		return SAM_STAT_RESERVATION_CONFLICT;
	return SAM_STAT_GOOD;
}

uint8_t ssc_erase(struct scsi_cmd *cmd)
{
	struct priv_lu_ssc *lu_priv;
	uint8_t *sam_stat;

	lu_priv = cmd->lu->lu_private;
	sam_stat = &cmd->dbuf_p->sam_stat;

	MHVTL_DBG(1, "Erasing (%ld) **", (long)cmd->dbuf_p->serialNo);

	current_state = MHVTL_STATE_ERASE;

	if (!lu_priv->pm->check_restrictions(cmd))
		return SAM_STAT_CHECK_CONDITION;

	if (c_pos->blk_number != 0) {
		MHVTL_LOG("Not at BOT.. Can't erase unless at BOT");
		mkSenseBuf(NOT_READY, E_INVALID_FIELD_IN_CDB, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	if (OK_to_write)
		format_tape(sam_stat);
	else {
		MHVTL_LOG("Attempt to erase Write-protected media");
		mkSenseBuf(NOT_READY, E_MEDIUM_OVERWRITE_ATTEMPTED, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}
	return SAM_STAT_GOOD;
}

uint8_t ssc_space(struct scsi_cmd *cmd)
{
	uint8_t *sam_stat;
	int count;
	int icount;
	int code;

	sam_stat = &cmd->dbuf_p->sam_stat;

	*sam_stat = SAM_STAT_GOOD;

	current_state = MHVTL_STATE_POSITIONING;

	count = get_unaligned_be24(&cmd->scb[2]);
	code = cmd->scb[1] & 0x07;

	/* 'count' is only a 24-bit value.  If the top bit is set, it
	   should be treated as a twos-complement negative number.
	*/

	if (count >= 0x800000)
		icount = -(0xffffff - count + 1);
	else
		icount = (int32_t)count;

	switch (code) {
	case 0:	/* Logical blocks - supported */
		MHVTL_DBG(1, "SPACE (%ld) ** %s %d block%s",
			(long)cmd->dbuf_p->serialNo,
			(icount >= 0) ? "forward" : "back",
			abs(icount),
			(1 == abs(icount)) ? "" : "s");
		break;
	case 1:	/* Filemarks - supported */
		MHVTL_DBG(1, "SPACE (%ld) ** %s %d filemark%s",
			(long)cmd->dbuf_p->serialNo,
			(icount >= 0) ? "forward" : "back",
			abs(icount),
			(1 == abs(icount)) ? "" : "s");
		break;
	case 3:	/* End of Data - supported */
		MHVTL_DBG(1, "SPACE (%ld) ** %s ",
			(long)cmd->dbuf_p->serialNo,
			"to End-of-data");
		break;
	case 2:	/* Sequential filemarks currently not supported */
	default: /* obsolete / reserved values */
		MHVTL_DBG(1, "SPACE (%ld) ** - Unsupported option %d",
			(long)cmd->dbuf_p->serialNo,
			code);

		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_PARMS, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
		break;
	}

	if (icount != 0 || code == 3)
		resp_space(icount, code, sam_stat);

	return *sam_stat;
}

uint8_t ssc_load_unload(struct scsi_cmd *cmd)
{
	struct priv_lu_ssc *lu_priv;
	uint8_t *sam_stat;
	int load;

	lu_priv = cmd->lu->lu_private;
	sam_stat = &cmd->dbuf_p->sam_stat;

	load = cmd->scb[4] & 0x01;

	current_state = (load) ? MHVTL_STATE_LOADING : MHVTL_STATE_UNLOADING;

	if (cmd->scb[4] & 0x04) { /* EOT bit */
		MHVTL_ERR("EOT bit set on load. Not supported");
		mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
	}

	MHVTL_DBG(1, "%s tape (%ld) **", (load) ? "Loading" : "Unloading",
						(long)cmd->dbuf_p->serialNo);

	switch (lu_priv->tapeLoaded) {
	case TAPE_UNLOADED:
		if (load)
			rewind_tape(sam_stat);
		else {
			mkSenseBuf(NOT_READY, E_MEDIUM_NOT_PRESENT, sam_stat);
			return SAM_STAT_CHECK_CONDITION;
		}
		break;

	case TAPE_LOADED:
		if (!load)
			unloadTape(sam_stat);
		break;

	default:
		mkSenseBuf(NOT_READY, E_MEDIUM_FMT_CORRUPT, sam_stat);
		return SAM_STAT_CHECK_CONDITION;
		break;
	}
	return SAM_STAT_GOOD;
}

uint8_t ssc_write_filemarks(struct scsi_cmd *cmd)
{
	struct priv_lu_ssc *lu_priv;
	uint8_t *sam_stat;
	int count;

	lu_priv = cmd->lu->lu_private;
	sam_stat = &cmd->dbuf_p->sam_stat;

	count = get_unaligned_be24(&cmd->scb[2]);

	MHVTL_DBG(1, "Write %d filemarks (%ld) **", count,
						(long)cmd->dbuf_p->serialNo);
	if (!lu_priv->pm->check_restrictions(cmd)) {
		/* If restrictions & WORM media at block 0.. OK
		 * Otherwise return CHECK_CONDITION.
		 *	check_restrictions()
		 *	was nice enough to set correct sense status for us.
		 */
		if ((mam.MediumType == MEDIA_TYPE_WORM) &&
					(c_pos->blk_number == 0)) {
			MHVTL_DBG(1, "Erasing WORM media");
		} else
			return SAM_STAT_CHECK_CONDITION;
	}

	write_filemarks(count, sam_stat);
	if (count) {
		if (current_tape_offset() >=
				get_unaligned_be64(&mam.max_capacity)) {
			mam.remaining_capacity = 0L;
			MHVTL_DBG(2, "Setting EOM flag");
			mkSenseBuf(NO_SENSE|SD_EOM, NO_ADDITIONAL_SENSE,
					sam_stat);
			return SAM_STAT_CHECK_CONDITION;
		}
	}

	return SAM_STAT_GOOD;
}

uint8_t ssc_pr_in(struct scsi_cmd *cmd)
{
	struct priv_lu_ssc *lu_priv;

	lu_priv = cmd->lu->lu_private;

	MHVTL_DBG(1, "PERSISTENT RESERVE IN (%ld) **",
						(long)cmd->dbuf_p->serialNo);
	if (lu_priv->I_am_SPC_2_Reserved)
		return SAM_STAT_RESERVATION_CONFLICT;
	else
		return resp_spc_pri(cmd->scb, cmd->dbuf_p);
}

static void update_seq_access_counters(struct seqAccessDevice *sa,
				struct priv_lu_ssc *lu_ssc)
{
	put_unaligned_be64(lu_ssc->bytesWritten_I,
				&sa->writeDataB4Compression);
	put_unaligned_be64(lu_ssc->bytesWritten_M,
				&sa->writeDataAfCompression);
	put_unaligned_be64(lu_ssc->bytesRead_M,
				&sa->readDataB4Compression);
	put_unaligned_be64(lu_ssc->bytesRead_I,
				&sa->readDataAfCompression);

	/* Values in MBytes */
	if (lu_ssc->tapeLoaded == TAPE_LOADED) {
		put_unaligned_be32(lu_ssc->max_capacity >> 20,
					&sa->capacity_bop_eod);
		put_unaligned_be32(lu_ssc->early_warning_position >> 20,
					&sa->capacity_bop_ew);
		put_unaligned_be32(lu_ssc->early_warning_sz >> 20,
					&sa->capacity_ew_leop);
		put_unaligned_be32(current_tape_offset() >> 20,
					&sa->capacity_bop_curr);
	} else {
		put_unaligned_be32(0xffffffff, &sa->capacity_bop_eod);
		put_unaligned_be32(0xffffffff, &sa->capacity_bop_ew);
		put_unaligned_be32(0xffffffff, &sa->capacity_ew_leop);
		put_unaligned_be32(0xffffffff, &sa->capacity_bop_curr);
	}

}

uint8_t ssc_log_sense(struct scsi_cmd *cmd)
{
	struct lu_phy_attr *lu;
	struct priv_lu_ssc *lu_ssc;
	uint8_t *b = cmd->dbuf_p->data;
	uint8_t *cdb = cmd->scb;
	uint8_t *sam_stat;
	int retval;
	int i;
	uint16_t alloc_len;
	struct list_head *l_head;
	struct log_pg_list *l;
	char msg[64];

	sprintf(msg, "LOG SENSE (%ld) ** : ", (long)cmd->dbuf_p->serialNo);

	alloc_len = get_unaligned_be16(&cdb[7]);
	cmd->dbuf_p->sz = alloc_len;

	lu = cmd->lu;
	lu_ssc = cmd->lu->lu_private;
	sam_stat = &cmd->dbuf_p->sam_stat;
	l_head = &lu->log_pg;
	retval = 0;

	switch (cdb[2] & 0x3f) {
	case 0:	/* Send supported pages */
		MHVTL_DBG(1, "%s %s", msg, "Sending supported pages");
		memset(b, 0, 4);	/* Clear first few (4) bytes */
		i = 4;
		b[i++] = 0;	/* b[0] is log page '0' (this one) */
		list_for_each_entry(l, l_head, siblings) {
			MHVTL_DBG(3, "found page 0x%02x", l->log_page_num);
			b[i] = l->log_page_num;
			i++;
		}
		put_unaligned_be16(i - 4, &b[2]);
		retval = i;
		break;
	case WRITE_ERROR_COUNTER:	/* Write error page */
		MHVTL_DBG(1, "%s %s", msg, "Write error page");
		l = lookup_log_pg(l_head, WRITE_ERROR_COUNTER);
		if (!l)
			goto log_page_not_found;

		b = memcpy(b, l->p, l->size);
		retval = l->size;
		break;
	case READ_ERROR_COUNTER:	/* Read error page */
		MHVTL_DBG(1, "%s %s", msg, "Read error page");
		l = lookup_log_pg(&lu->log_pg, READ_ERROR_COUNTER);
		if (!l)
			goto log_page_not_found;

		b = memcpy(b, l->p, l->size);
		retval = l->size;
		break;
	case SEQUENTIAL_ACCESS_DEVICE:
		MHVTL_DBG(1, "%s %s", msg, "Sequential Access Device Log page");
		l = lookup_log_pg(&lu->log_pg, SEQUENTIAL_ACCESS_DEVICE);
		if (!l)
			goto log_page_not_found;

		b = memcpy(b, l->p, l->size);
		update_seq_access_counters((struct seqAccessDevice *)b, lu_ssc);
		retval = l->size;
		break;
	case TEMPERATURE_PAGE:	/* Temperature page */
		MHVTL_DBG(1, "%s %s", msg, "Temperature page");
		l = lookup_log_pg(&lu->log_pg, TEMPERATURE_PAGE);
		if (!l)
			goto log_page_not_found;

		b = memcpy(b, l->p, l->size);
		retval = l->size;
		break;
	case TAPE_ALERT:	/* TapeAlert page */
		MHVTL_DBG(1, "%s %s", msg, "TapeAlert page");

		l = lookup_log_pg(&lu->log_pg, TAPE_ALERT);
		if (!l)
			goto log_page_not_found;

		b = memcpy(b, l->p, l->size);
		retval = l->size;

		/* Clear flags after value read. */
		if (alloc_len > 4)
			update_TapeAlert(lu, 0);
		else
			MHVTL_DBG(1, "TapeAlert : Alloc len short -"
				" Not clearing TapeAlert flags.");
		break;
	case TAPE_USAGE:	/* Tape Usage Log */
		MHVTL_DBG(1, "%s %s", msg, "Tape Usage page");
		l = lookup_log_pg(&lu->log_pg, TAPE_USAGE);
		if (!l)
			goto log_page_not_found;

		b = memcpy(b, l->p, l->size);
		retval = l->size;
		break;
	case TAPE_CAPACITY: {	/* Tape Capacity page */
		MHVTL_DBG(1, "%s %s", msg, "Tape Capacity page");
		struct TapeCapacity *tp;

		l = lookup_log_pg(&lu->log_pg, TAPE_CAPACITY);
		if (!l)
			goto log_page_not_found;

		b = memcpy(b, l->p, l->size);
		retval = l->size;

		/* Point the data structure to return data */
		tp = (struct TapeCapacity *)b;

		if (lu_ssc->tapeLoaded == TAPE_LOADED) {
			uint64_t cap;

			cap = get_unaligned_be64(&mam.remaining_capacity);
			cap /= lu_ssc->capacity_unit;
			put_unaligned_be32(cap, &tp->value01);

			cap = get_unaligned_be64(&mam.max_capacity);
			cap /= lu_ssc->capacity_unit;
			put_unaligned_be32(cap, &tp->value03);
		} else {
			tp->value01 = 0;
			tp->value03 = 0;
		}
		}
		break;
	case DATA_COMPRESSION:	/* Data Compression page */
		MHVTL_DBG(1, "%s %s", msg, "Data Compression page");
		l = lookup_log_pg(&lu->log_pg, DATA_COMPRESSION);
		if (!l)
			goto log_page_not_found;

		b = memcpy(b, l->p, l->size);
		retval = l->size;
		break;
	default:
		MHVTL_DBG(1, "%s Unknown code: 0x%x", msg, cdb[2] & 0x3f);
		goto log_page_not_found;
		break;
	}
	cmd->dbuf_p->sz = retval;

	return SAM_STAT_GOOD;

log_page_not_found:
	cmd->dbuf_p->sz = 0;
	mkSenseBuf(ILLEGAL_REQUEST, E_INVALID_FIELD_IN_CDB, sam_stat);
	return SAM_STAT_CHECK_CONDITION;
}

