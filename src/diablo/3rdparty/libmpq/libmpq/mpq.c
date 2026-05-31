/*
 *  mpq.c -- functions for developers using libmpq.
 *
 *  Copyright (c) 2003-2011 Maik Broemme <mbroemme@libmpq.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

/* mpq-tools configuration includes. */
#include "config.h"

/* libmpq main includes. */
#include "mpq.h"
#include "mpq-internal.h"

/* libmpq generic includes. */
#include "common.h"
#include "endian.h"
#include "explode.h"
#include "huffman.h"

/* generic includes. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* support for platform specific things */
#include "platform.h"

#if (defined(_WIN64) || defined(_WIN32)) && !defined(NXDK)
/* Suppress definitions of `min` and `max` macros by <windows.h>: */
#define NOMINMAX 1
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlwapi.h>
#endif

/* static error constants. */
static const char *__libmpq_error_strings[] = {
	"success",
	"open error on file",
	"close error on file",
	"lseek error on file",
	"read error on file",
	"write error on file",
	"memory allocation error",
	"format errror",
	"init() wasn't called",
	"buffer size is to small",
	"file or block does not exist in archive",
	"we don't know the decryption seed",
	"error on unpacking file"
};

/* this function returns the library version information. */
const char *libmpq__version(void) {

	/* return version information. */
	return VERSION;
}

/* this function returns a string message for a return code. */
const char *libmpq__strerror(int32_t return_code) {

	/* check for array bounds */
	if (-return_code < 0 || -return_code > sizeof(__libmpq_error_strings)/sizeof(char*))
		return NULL;

	/* return appropriate string */
	return __libmpq_error_strings[-return_code];
}

#if (defined(_WIN64) || defined(_WIN32)) && !defined(NXDK)
wchar_t *to_wide_char(const char *str) {
	const size_t size = strlen(str);
	const uint32_t flags = MB_ERR_INVALID_CHARS;
	const int wide_size = MultiByteToWideChar(CP_UTF8, flags, str, size, NULL, 0);
	if (wide_size == 0)
		return NULL;
	wchar_t *result = malloc(sizeof(wchar_t) * (wide_size + 1));
	if (result == NULL) {
		return NULL;
	}
	if (MultiByteToWideChar(CP_UTF8, flags, str, size, result, wide_size) != wide_size) {
		free(result);
		return NULL;
	}
	result[wide_size] = L'\0';
	return result;
}
#endif

static FILE *fopen_utf8(const char *filename) {
#if (defined(_WIN64) || defined(_WIN32)) && !defined(NXDK)
	wchar_t *filename_wide = to_wide_char(filename);
	if (filename_wide == NULL) return NULL;
	FILE *result = _wfopen(filename_wide, L"rb");
	free(filename_wide);
	filename_wide = NULL;
	return result;
#else
	return fopen(filename, "rb");
#endif
}

static void bswap_mpq_header(mpq_header_s *mpq_header) {
	mpq_header->mpq_magic = libmpq__bswap_LE32(mpq_header->mpq_magic);
	mpq_header->header_size = libmpq__bswap_LE32(mpq_header->header_size);
	mpq_header->archive_size = libmpq__bswap_LE32(mpq_header->archive_size);
	mpq_header->version = libmpq__bswap_LE16(mpq_header->version);
	mpq_header->block_size = libmpq__bswap_LE16(mpq_header->block_size);
	mpq_header->hash_table_offset = libmpq__bswap_LE32(mpq_header->hash_table_offset);
	mpq_header->block_table_offset = libmpq__bswap_LE32(mpq_header->block_table_offset);
	mpq_header->hash_table_count = libmpq__bswap_LE32(mpq_header->hash_table_count);
	mpq_header->block_table_count = libmpq__bswap_LE32(mpq_header->block_table_count);
}

static void bswap_mpq_header_ex(mpq_header_ex_s *mpq_header_ex) {
	mpq_header_ex->extended_offset = libmpq__bswap_LE64(mpq_header_ex->extended_offset);
	mpq_header_ex->hash_table_offset_high = libmpq__bswap_LE16(mpq_header_ex->hash_table_offset_high);
	mpq_header_ex->block_table_offset_high = libmpq__bswap_LE16(mpq_header_ex->block_table_offset_high);
}

static void bswap_mpq_hash(mpq_hash_s *mpq_hash, size_t count) {
	for (size_t i = 0; i < count; ++i) {
		mpq_hash[i].hash_a = libmpq__bswap_LE32(mpq_hash[i].hash_a);
		mpq_hash[i].hash_b = libmpq__bswap_LE32(mpq_hash[i].hash_b);
		mpq_hash[i].locale = libmpq__bswap_LE16(mpq_hash[i].locale);
		mpq_hash[i].platform = libmpq__bswap_LE16(mpq_hash[i].platform);
		mpq_hash[i].block_table_index = libmpq__bswap_LE32(mpq_hash[i].block_table_index);
	}
}

static void bswap_mpq_block(mpq_block_s *mpq_block, size_t count) {
	for (size_t i = 0; i < count; ++i) {
		mpq_block[i].offset = libmpq__bswap_LE32(mpq_block[i].offset);
		mpq_block[i].packed_size = libmpq__bswap_LE32(mpq_block[i].packed_size);
		mpq_block[i].unpacked_size = libmpq__bswap_LE32(mpq_block[i].unpacked_size);
		mpq_block[i].flags = libmpq__bswap_LE32(mpq_block[i].flags);
	}
}

static void bswap_mpq_block_ex(mpq_block_ex_s *mpq_block_ex, size_t count) {
	for (size_t i = 0; i < count; ++i) {
		mpq_block_ex[i].offset_high = libmpq__bswap_LE16(mpq_block_ex[i].offset_high);
	}
}

static void bswap_uint32s(uint32_t *values, size_t count) {
	for (size_t i = 0; i < count; ++i) {
		values[i] = libmpq__bswap_LE32(values[i]);
	}
}

static int32_t get_decompression_work_buf_pkzip(mpq_archive_s *mpq_archive, uint8_t **out) {
	// The multi buffer is larger so we use it instead.
	if (mpq_archive->decompress_work_buf_multi != NULL) {
		*out = mpq_archive->decompress_work_buf_multi;
		return 0;
	}
	if (mpq_archive->decompress_work_buf_pkzip == NULL) {
		if ((mpq_archive->decompress_work_buf_pkzip = malloc(sizeof(pkzip_cmp_s))) == NULL) {
			return LIBMPQ_ERROR_MALLOC;
		}
	}
	*out = mpq_archive->decompress_work_buf_pkzip;
	return 0;
}

static int32_t get_decompression_work_buf_multi(mpq_archive_s *mpq_archive, uint8_t **out) {
	if (mpq_archive->decompress_work_buf_multi == NULL) {
		if ((mpq_archive->decompress_work_buf_multi = malloc(sizeof(struct huffman_tree_s))) == NULL) {
			return LIBMPQ_ERROR_MALLOC;
		}
		// The multi buffer will be used for pkzip as well because it is larger.
		free(mpq_archive->decompress_work_buf_pkzip);
		mpq_archive->decompress_work_buf_pkzip = NULL;
	}
	*out = mpq_archive->decompress_work_buf_multi;
	return 0;
}

/* this function read a file and verify if it is a valid mpq archive, then it read and decrypt the hash table. */
int32_t libmpq__archive_open(mpq_archive_s **mpq_archive, const char *mpq_filename, libmpq__off_t archive_offset) {

	/* some common variables. */
	uint32_t i             = 0;
	uint32_t count         = 0;
	int32_t result         = 0;
	uint32_t header_search = FALSE;

	if (archive_offset == -1) {
		archive_offset = 0;
		header_search = TRUE;
	}

	if ((*mpq_archive = calloc(1, sizeof(mpq_archive_s))) == NULL) {

		/* archive struct could not be allocated */
		return LIBMPQ_ERROR_MALLOC;
	}

	/* check if file exists and is readable */
	errno = 0;
	if (((*mpq_archive)->fp = fopen_utf8(mpq_filename)) == NULL) {

		/* file could not be opened. */
		result = errno == ENOENT ? LIBMPQ_ERROR_EXIST : LIBMPQ_ERROR_OPEN;
		goto error;
	}

#ifdef LIBMPQ_FILE_BUFFER_SIZE
	if (setvbuf((*mpq_archive)->fp, NULL, _IOFBF, LIBMPQ_FILE_BUFFER_SIZE) != 0) {

		/* buffer size could not be set */
		result = LIBMPQ_ERROR_OPEN;
		goto error;
	}
#endif

	/* assign some default values. */
	(*mpq_archive)->mpq_header.mpq_magic = 0;
	(*mpq_archive)->files                = 0;

	/* loop through file and search for mpq signature. */
	while (TRUE) {

		/* reset header values. */
		(*mpq_archive)->mpq_header.mpq_magic = 0;

		/* seek in file. */
		if (fseeko((*mpq_archive)->fp, archive_offset, SEEK_SET) < 0) {

			/* seek in file failed. */
			result = LIBMPQ_ERROR_SEEK;
			goto error;
		}

		/* read header from file. */
		if (fread(&(*mpq_archive)->mpq_header, 1, sizeof(mpq_header_s), (*mpq_archive)->fp) != sizeof(mpq_header_s)) {

			/* no valid mpq archive. */
			result = LIBMPQ_ERROR_FORMAT;
			goto error;
		}

		bswap_mpq_header(&(*mpq_archive)->mpq_header);

		/* check if we found a valid mpq header. */
		if ((*mpq_archive)->mpq_header.mpq_magic == LIBMPQ_HEADER) {

			/* check if we process old mpq archive version. */
			if ((*mpq_archive)->mpq_header.version == LIBMPQ_ARCHIVE_VERSION_ONE) {

				/* check if the archive is protected. */
				if ((*mpq_archive)->mpq_header.header_size != sizeof(mpq_header_s)) {

					/* correct header size. */
					(*mpq_archive)->mpq_header.header_size = sizeof(mpq_header_s);
				}
			}

			/* check if we process new mpq archive version. */
			if ((*mpq_archive)->mpq_header.version == LIBMPQ_ARCHIVE_VERSION_TWO) {

				/* check if the archive is protected. */
				if ((*mpq_archive)->mpq_header.header_size != sizeof(mpq_header_s) + sizeof(mpq_header_ex_s)) {

					/* correct header size. */
					(*mpq_archive)->mpq_header.header_size = sizeof(mpq_header_s) + sizeof(mpq_header_ex_s);
				}
			}

			/* break the loop, because header was found. */
			break;
		}

		/* move to the next possible offset. */
		if (!header_search) {

			/* no valid mpq archive. */
			result = LIBMPQ_ERROR_FORMAT;
			goto error;
		}
		archive_offset += 512;
	}

	/* store block size for later use. */
	(*mpq_archive)->block_size = 512 << (*mpq_archive)->mpq_header.block_size;

	/* store archive offset and size for later use. */
	(*mpq_archive)->archive_offset = archive_offset;

	/* check if we process new mpq archive version. */
	if ((*mpq_archive)->mpq_header.version == LIBMPQ_ARCHIVE_VERSION_TWO) {

		/* seek in file. */
		if (fseeko((*mpq_archive)->fp, sizeof(mpq_header_s) + archive_offset, SEEK_SET) < 0) {

			/* seek in file failed. */
			result = LIBMPQ_ERROR_SEEK;
			goto error;
		}

		/* read header from file. */
		if (fread(&(*mpq_archive)->mpq_header_ex, 1, sizeof(mpq_header_ex_s), (*mpq_archive)->fp) != sizeof(mpq_header_ex_s)) {

			/* no valid mpq archive. */
			result = LIBMPQ_ERROR_FORMAT;
			goto error;
		}
		bswap_mpq_header_ex(&(*mpq_archive)->mpq_header_ex);
	}

	/* allocate memory for the block table, hash table, file and block table to file mapping. */
	if (((*mpq_archive)->mpq_block    = calloc((*mpq_archive)->mpq_header.block_table_count, sizeof(mpq_block_s))) == NULL ||
	    ((*mpq_archive)->mpq_block_ex = calloc((*mpq_archive)->mpq_header.block_table_count, sizeof(mpq_block_ex_s))) == NULL ||
	    ((*mpq_archive)->mpq_hash     = calloc((*mpq_archive)->mpq_header.hash_table_count,  sizeof(mpq_hash_s))) == NULL ||
	    ((*mpq_archive)->mpq_file     = calloc((*mpq_archive)->mpq_header.block_table_count, sizeof(mpq_file_s *))) == NULL ||
	    ((*mpq_archive)->mpq_map      = calloc((*mpq_archive)->mpq_header.block_table_count, sizeof(mpq_map_s))) == NULL) {

		/* memory allocation problem. */
		result = LIBMPQ_ERROR_MALLOC;
		goto error;
	}

	/* seek in file. */
	if (fseeko((*mpq_archive)->fp, (*mpq_archive)->mpq_header.hash_table_offset + (((long long)((*mpq_archive)->mpq_header_ex.hash_table_offset_high)) << 32) + (*mpq_archive)->archive_offset, SEEK_SET) < 0) {

		/* seek in file failed. */
		result = LIBMPQ_ERROR_SEEK;
		goto error;
	}

	/* read the hash table into the buffer. */
	if (fread((*mpq_archive)->mpq_hash, 1, (*mpq_archive)->mpq_header.hash_table_count * sizeof(mpq_hash_s), (*mpq_archive)->fp) != (*mpq_archive)->mpq_header.hash_table_count * sizeof(mpq_hash_s)) {

		/* something on read failed. */
		result = LIBMPQ_ERROR_READ;
		goto error;
	}

	/* convert loaded data to platform endian */
	bswap_uint32s((uint32_t *)((*mpq_archive)->mpq_hash), (*mpq_archive)->mpq_header.hash_table_count * sizeof(mpq_hash_s) / sizeof(uint32_t));

	/* decrypt the hashtable. */
	libmpq__decrypt_block((uint32_t *)((*mpq_archive)->mpq_hash), (*mpq_archive)->mpq_header.hash_table_count * sizeof(mpq_hash_s), libmpq__hash_string("(hash table)", 0x300));

	/* convert decrypted data to original endian */
	bswap_uint32s((uint32_t *)((*mpq_archive)->mpq_hash), (*mpq_archive)->mpq_header.hash_table_count * sizeof(mpq_hash_s) / sizeof(uint32_t));

	/* cast array of uint32_t to mpq_hash_s and convert to platform endian */
	bswap_mpq_hash((*mpq_archive)->mpq_hash, (*mpq_archive)->mpq_header.hash_table_count);

	/* seek in file. */
	if (fseeko((*mpq_archive)->fp, (*mpq_archive)->mpq_header.block_table_offset + (((long long)((*mpq_archive)->mpq_header_ex.block_table_offset_high)) << 32) + (*mpq_archive)->archive_offset, SEEK_SET) < 0) {

		/* seek in file failed. */
		result = LIBMPQ_ERROR_SEEK;
		goto error;
	}

	/* read the block table into the buffer. */
	if (fread((*mpq_archive)->mpq_block, 1, (*mpq_archive)->mpq_header.block_table_count * sizeof(mpq_block_s), (*mpq_archive)->fp) != (*mpq_archive)->mpq_header.block_table_count * sizeof(mpq_block_s)) {

		/* something on read failed. */
		result = LIBMPQ_ERROR_READ;
		goto error;
	}

	/* convert loaded data to platform endian */
	bswap_uint32s((uint32_t *)((*mpq_archive)->mpq_block), (*mpq_archive)->mpq_header.block_table_count * sizeof(mpq_block_s) / sizeof(uint32_t));

	/* decrypt block table. */
	libmpq__decrypt_block((uint32_t *)((*mpq_archive)->mpq_block), (*mpq_archive)->mpq_header.block_table_count * sizeof(mpq_block_s), libmpq__hash_string("(block table)", 0x300));

	/*
	 * mpq_block_s is a block of uint32_t values, so we can skip convertion to orignal endian, casting and conversion to platform endian.
	 */

	/* check if extended block table is present, regardless of version 2 it is only present in archives > 4GB. */
	if ((*mpq_archive)->mpq_header_ex.extended_offset > 0) {

		/* seek in file. */
		if (fseeko((*mpq_archive)->fp, (*mpq_archive)->mpq_header_ex.extended_offset + archive_offset, SEEK_SET) < 0) {

			/* seek in file failed. */
			result = LIBMPQ_ERROR_SEEK;
			goto error;
		}

		/* read header from file. */
		if (fread((*mpq_archive)->mpq_block_ex, 1, (*mpq_archive)->mpq_header.block_table_count * sizeof(mpq_block_ex_s), (*mpq_archive)->fp) != (*mpq_archive)->mpq_header.block_table_count * sizeof(mpq_block_ex_s)) {

			/* no valid mpq archive. */
			result = LIBMPQ_ERROR_FORMAT;
			goto error;
		}
		bswap_mpq_block_ex((*mpq_archive)->mpq_block_ex, (*mpq_archive)->mpq_header.block_table_count);
	}

	/* loop through all files in mpq archive and check if they are valid. */
	for (i = 0; i < (*mpq_archive)->mpq_header.block_table_count; i++) {

		/* save block difference between valid and invalid blocks. */
		(*mpq_archive)->mpq_map[i].block_table_diff = i - count;

		/* check if file exists, sizes and offsets are correct. */
		if (((*mpq_archive)->mpq_block[i].flags & LIBMPQ_FLAG_EXISTS) == 0) {

			/* file does not exist, so nothing to do with that block. */
			continue;
		}

		/* create final indices tables. */
		(*mpq_archive)->mpq_map[count].block_table_indices = i;

		/* increase file counter. */
		count++;
	}

	/* save the number of files. */
	(*mpq_archive)->files = count;

	/* if no error was found, return zero. */
	return LIBMPQ_SUCCESS;

error:
	if ((*mpq_archive)->fp)
		fclose((*mpq_archive)->fp);

	free((*mpq_archive)->mpq_map);
	free((*mpq_archive)->mpq_file);
	free((*mpq_archive)->mpq_hash);
	free((*mpq_archive)->mpq_block);
	free((*mpq_archive)->mpq_block_ex);
	free(*mpq_archive);

	*mpq_archive = NULL;

	return result;
}

/* duplicates an mpq archive. */
int32_t libmpq__archive_dup(mpq_archive_s *orig_archive, const char *mpq_filename, mpq_archive_s **mpq_archive) {
	int32_t result = 0;

	if ((*mpq_archive = calloc(1, sizeof(mpq_archive_s))) == NULL) {

		/* archive struct could not be allocated */
		return LIBMPQ_ERROR_MALLOC;
	}

	/* check if file exists and is readable */
	if (((*mpq_archive)->fp = fopen_utf8(mpq_filename)) == NULL) {

		/* file could not be opened. */
		result = errno == ENOENT ? LIBMPQ_ERROR_EXIST : LIBMPQ_ERROR_OPEN;
		goto error;
	}

#ifdef LIBMPQ_FILE_BUFFER_SIZE
	if (setvbuf((*mpq_archive)->fp, NULL, _IOFBF, LIBMPQ_FILE_BUFFER_SIZE) != 0) {

		/* buffer size could not be set */
		result = LIBMPQ_ERROR_OPEN;
		goto error;
	}
#endif

	(*mpq_archive)->block_size = orig_archive->block_size;
	(*mpq_archive)->archive_offset = orig_archive->archive_offset;
	(*mpq_archive)->mpq_header = orig_archive->mpq_header;
	(*mpq_archive)->mpq_header_ex = orig_archive->mpq_header_ex;
	(*mpq_archive)->files = orig_archive->files;

	const mpq_header_s *header = &(*mpq_archive)->mpq_header;

	/* allocate memory for the block table, hash table, file and block table to file mapping. */
	if (((*mpq_archive)->mpq_block    = malloc(header->block_table_count * sizeof(mpq_block_s))) == NULL ||
	    ((*mpq_archive)->mpq_block_ex = malloc(header->block_table_count * sizeof(mpq_block_ex_s))) == NULL ||
	    ((*mpq_archive)->mpq_hash     = malloc(header->hash_table_count  * sizeof(mpq_hash_s))) == NULL ||
	    ((*mpq_archive)->mpq_file     = calloc(header->block_table_count, sizeof(mpq_file_s *))) == NULL ||
	    ((*mpq_archive)->mpq_map      = malloc(header->block_table_count * sizeof(mpq_map_s))) == NULL) {

		/* memory allocation problem. */
		result = LIBMPQ_ERROR_MALLOC;
		goto error;
	}

	memcpy((*mpq_archive)->mpq_block, orig_archive->mpq_block, header->block_table_count * sizeof(mpq_block_s));
	memcpy((*mpq_archive)->mpq_block_ex, orig_archive->mpq_block_ex, header->block_table_count * sizeof(mpq_block_ex_s));
	memcpy((*mpq_archive)->mpq_hash, orig_archive->mpq_hash, header->block_table_count * sizeof(mpq_hash_s));
	memcpy((*mpq_archive)->mpq_map, orig_archive->mpq_map, header->block_table_count * sizeof(mpq_map_s));

	return LIBMPQ_SUCCESS;
error:
	if ((*mpq_archive)->fp)
		fclose((*mpq_archive)->fp);

	free((*mpq_archive)->mpq_map);
	free((*mpq_archive)->mpq_file);
	free((*mpq_archive)->mpq_hash);
	free((*mpq_archive)->mpq_block);
	free((*mpq_archive)->mpq_block_ex);
	free(*mpq_archive);

	*mpq_archive = NULL;

	return result;
}

/* this function close the file descriptor, free the decryption buffer and the file list. */
int32_t libmpq__archive_close(mpq_archive_s *mpq_archive) {

	/* try to close the file */
	if ((fclose(mpq_archive->fp)) < 0) {

		/* don't free anything here, so the caller can try calling us
		 * again.
		 */
		return LIBMPQ_ERROR_CLOSE;
	}

	/* free header, tables and list. */
	free(mpq_archive->mpq_map);
	free(mpq_archive->mpq_file);
	free(mpq_archive->mpq_hash);
	free(mpq_archive->mpq_block);
	free(mpq_archive->mpq_block_ex);
	free(mpq_archive->decompress_work_buf_pkzip);
	free(mpq_archive->decompress_work_buf_multi);
	free(mpq_archive);

	/* if no error was found, return zero. */
	return LIBMPQ_SUCCESS;
}

/* this function return the packed size of all files in the archive. */
int32_t libmpq__archive_size_packed(mpq_archive_s *mpq_archive, libmpq__off_t *packed_size) {

	/* some common variables. */
	uint32_t i;

	/* loop through all files in archive and count packed size. */
	for (i = 0; i < mpq_archive->files; i++) {
		*packed_size += mpq_archive->mpq_block[mpq_archive->mpq_map[i].block_table_indices].packed_size;
	}

	/* if no error was found, return zero. */
	return LIBMPQ_SUCCESS;
}

/* this function return the unpacked size of all files in the archive. */
int32_t libmpq__archive_size_unpacked(mpq_archive_s *mpq_archive, libmpq__off_t *unpacked_size) {

	/* some common variables. */
	uint32_t i;

	/* loop through all files in archive and count unpacked size. */
	for (i = 0; i < mpq_archive->files; i++) {
		*unpacked_size += mpq_archive->mpq_block[mpq_archive->mpq_map[i].block_table_indices].unpacked_size;
	}

	/* if no error was found, return zero. */
	return LIBMPQ_SUCCESS;
}

/* this function return the archive offset (beginning of archive in file). */
int32_t libmpq__archive_offset(mpq_archive_s *mpq_archive, libmpq__off_t *offset) {

	/* return archive offset. */
	*offset = mpq_archive->archive_offset;

	/* if no error was found, return zero. */
	return LIBMPQ_SUCCESS;
}

/* this function return the archive offset. */
int32_t libmpq__archive_version(mpq_archive_s *mpq_archive, uint32_t *version) {

	/* return archive version. */
	*version = mpq_archive->mpq_header.version + 1;

	/* if no error was found, return zero. */
	return LIBMPQ_SUCCESS;
}

/* this function return the number of valid files in archive. */
int32_t libmpq__archive_files(mpq_archive_s *mpq_archive, uint32_t *files) {

	/* return archive version. */
	*files = mpq_archive->files;

	/* if no error was found, return zero. */
	return LIBMPQ_SUCCESS;
}

#define CHECK_FILE_NUM(file_number, mpq_archive) \
	if (file_number < 0 || file_number > mpq_archive->files - 1) { \
		return LIBMPQ_ERROR_EXIST; \
	}

#define CHECK_BLOCK_NUM(block_number, mpq_archive) \
	if (block_number < 0 || block_number >= ((mpq_archive->mpq_block[mpq_archive->mpq_map[file_number].block_table_indices].flags & LIBMPQ_FLAG_SINGLE) != 0 ? 1 : (mpq_archive->mpq_block[mpq_archive->mpq_map[file_number].block_table_indices].unpacked_size + mpq_archive->block_size - 1) / mpq_archive->block_size)) { \
		return LIBMPQ_ERROR_EXIST; \
	}

/* this function return the packed size of the given files in the archive. */
int32_t libmpq__file_size_packed(mpq_archive_s *mpq_archive, uint32_t file_number, libmpq__off_t *packed_size) {

	/* check if given file number is not out of range. */
	CHECK_FILE_NUM(file_number, mpq_archive)

	/* get the packed size of file. */
	*packed_size = mpq_archive->mpq_block[mpq_archive->mpq_map[file_number].block_table_indices].packed_size;

	/* if no error was found, return zero. */
	return LIBMPQ_SUCCESS;
}

/* this function return the unpacked size of the given file in the archive. */
int32_t libmpq__file_size_unpacked(mpq_archive_s *mpq_archive, uint32_t file_number, libmpq__off_t *unpacked_size) {

	/* check if given file number is not out of range. */
	CHECK_FILE_NUM(file_number, mpq_archive)

	/* get the unpacked size of file. */
	*unpacked_size = mpq_archive->mpq_block[mpq_archive->mpq_map[file_number].block_table_indices].unpacked_size;

	/* if no error was found, return zero. */
	return LIBMPQ_SUCCESS;
}

/* this function return the file offset (beginning of file in archive). */
int32_t libmpq__file_offset(mpq_archive_s *mpq_archive, uint32_t file_number, libmpq__off_t *offset) {

	/* check if given file number is not out of range. */
	CHECK_FILE_NUM(file_number, mpq_archive)

	/* return file offset relative to archive start. */
	*offset = mpq_archive->mpq_block[mpq_archive->mpq_map[file_number].block_table_indices].offset + (((long long)mpq_archive->mpq_block_ex[mpq_archive->mpq_map[file_number].block_table_indices].offset_high) << 32);

	/* if no error was found, return zero. */
	return LIBMPQ_SUCCESS;
}

/* this function return the number of blocks for the given file in the archive. */
int32_t libmpq__file_blocks(mpq_archive_s *mpq_archive, uint32_t file_number, uint32_t *blocks) {

	/* check if given file number is not out of range. */
	CHECK_FILE_NUM(file_number, mpq_archive)

	/* return the number of blocks for the given file. */
	*blocks = (mpq_archive->mpq_block[mpq_archive->mpq_map[file_number].block_table_indices].flags & LIBMPQ_FLAG_SINGLE) != 0 ? 1 : (mpq_archive->mpq_block[mpq_archive->mpq_map[file_number].block_table_indices].unpacked_size + mpq_archive->block_size - 1) / mpq_archive->block_size;

	/* if no error was found, return zero. */
	return LIBMPQ_SUCCESS;
}

/* this function return if the file is encrypted or not. */
int32_t libmpq__file_encrypted(mpq_archive_s *mpq_archive, uint32_t file_number, uint32_t *encrypted) {

	/* check if given file number is not out of range. */
	CHECK_FILE_NUM(file_number, mpq_archive)

	/* return the encryption status of file. */
	*encrypted = (mpq_archive->mpq_block[mpq_archive->mpq_map[file_number].block_table_indices].flags & LIBMPQ_FLAG_ENCRYPTED) != 0 ? TRUE : FALSE;

	/* if no error was found, return zero. */
	return LIBMPQ_SUCCESS;
}

/* this function return if the file is compressed or not. */
int32_t libmpq__file_compressed(mpq_archive_s *mpq_archive, uint32_t file_number, uint32_t *compressed) {

	/* check if given file number is not out of range. */
	CHECK_FILE_NUM(file_number, mpq_archive)

	/* return the compression status of file. */
	*compressed = (mpq_archive->mpq_block[mpq_archive->mpq_map[file_number].block_table_indices].flags & LIBMPQ_FLAG_COMPRESS_MULTI) != 0 ? TRUE : FALSE;

	/* if no error was found, return zero. */
	return LIBMPQ_SUCCESS;
}

/* this function return if the file is imploded or not. */
int32_t libmpq__file_imploded(mpq_archive_s *mpq_archive, uint32_t file_number, uint32_t *imploded) {

	/* check if given file number is not out of range. */
	CHECK_FILE_NUM(file_number, mpq_archive)

	/* return the implosion status of file. */
	*imploded = (mpq_archive->mpq_block[mpq_archive->mpq_map[file_number].block_table_indices].flags & LIBMPQ_FLAG_COMPRESS_PKZIP) != 0 ? TRUE : FALSE;

	/* if no error was found, return zero. */
	return LIBMPQ_SUCCESS;
}

/* calculates the filename hash. */
void libmpq__file_hash(const char *filename, uint32_t *hash1, uint32_t *hash2, uint32_t *hash3) {
	*hash1 = libmpq__hash_string(filename, 0x0);
	*hash2 = libmpq__hash_string(filename, 0x100);
	*hash3 = libmpq__hash_string(filename, 0x200);
}

/* finds the file number with the given filename hash. */
int32_t libmpq__file_number_from_hash(mpq_archive_s *mpq_archive, uint32_t hash1, uint32_t hash2, uint32_t hash3, uint32_t *number) {
	/* if the list of file names doesn't include this one, we'll have
	 * to figure out the file number the "hard" way.
	 */
	uint32_t ht_count = mpq_archive->mpq_header.hash_table_count;

	hash1 &= (ht_count - 1);

	/* loop through all files in mpq archive.
	 * hash1 gives us a clue about the starting position of this
	 * search.
	 */
	for (uint32_t i = hash1; mpq_archive->mpq_hash[i].block_table_index != LIBMPQ_HASH_FREE; i = (i + 1) & (ht_count - 1)) {

		/* if the other two hashes match, we found our file number. */
		if (mpq_archive->mpq_hash[i].block_table_index != LIBMPQ_HASH_DELETED &&
			mpq_archive->mpq_hash[i].hash_a == hash2 &&
		    mpq_archive->mpq_hash[i].hash_b == hash3) {

			/* return the file number. */
			*number = mpq_archive->mpq_hash[i].block_table_index - mpq_archive->mpq_map[mpq_archive->mpq_hash[i].block_table_index].block_table_diff;

			/* we found our file, return zero. */
			return LIBMPQ_SUCCESS;
		}

		/* check if we have cycled through the whole hash table */
		if (((i + 1) & (ht_count - 1)) == hash1) {
			break;
		}
	}

	/* if no matching entry found, so return error. */
	return LIBMPQ_ERROR_EXIST;
}


/* this function return filenumber by the given name. */
int32_t libmpq__file_number(mpq_archive_s *mpq_archive, const char *filename, uint32_t *number) {
	uint32_t hash1, hash2, hash3;
	libmpq__file_hash(filename, &hash1, &hash2, &hash3);
	return libmpq__file_number_from_hash(mpq_archive, hash1, hash2, hash3, number);
}

/* this function read the given file from archive into a buffer. */
int32_t libmpq__file_read(mpq_archive_s *mpq_archive, uint32_t file_number, uint8_t *out_buf, libmpq__off_t out_size, libmpq__off_t *transferred) {
	int32_t result = 0;

	if ((result = libmpq__block_open_offset(mpq_archive, file_number)) < 0) {
		return result;
	}

	libmpq__off_t unpacked_size;
	if ((result = libmpq__block_size_unpacked(mpq_archive, file_number, 0, &unpacked_size)) != 0) {
		return result;
	}

	uint8_t *tmp_buf = calloc(unpacked_size, sizeof(uint8_t));
	
	result = libmpq__file_read_with_temporary_buffer(mpq_archive, file_number, out_buf, out_size, tmp_buf, unpacked_size, transferred);
	
	free(tmp_buf);
	libmpq__block_close_offset(mpq_archive, file_number);

	return result;
}

int32_t libmpq__file_read_with_filename(mpq_archive_s *mpq_archive, uint32_t file_number, const char *filename, uint8_t *out_buf, libmpq__off_t out_size, libmpq__off_t *transferred) {
	int32_t result = 0;

	if ((result = libmpq__block_open_offset_with_filename(mpq_archive, file_number, filename)) < 0) {
		return result;
	}

	libmpq__off_t unpacked_size;
	if ((result = libmpq__block_size_unpacked(mpq_archive, file_number, 0, &unpacked_size)) != 0) {
		return result;
	}

	uint8_t *tmp_buf = calloc(unpacked_size, sizeof(uint8_t));
	
	result = libmpq__file_read_with_temporary_buffer(mpq_archive, file_number, out_buf, out_size, tmp_buf, unpacked_size, transferred);
	
	free(tmp_buf);
	libmpq__block_close_offset(mpq_archive, file_number);

	return result;
}

/* this function read the given file from archive into a buffer using a user-provided temporary buffer. */
int32_t libmpq__file_read_with_temporary_buffer(mpq_archive_s *mpq_archive, uint32_t file_number, uint8_t *out_buf, libmpq__off_t out_size, uint8_t *tmp_buf, libmpq__off_t tmp_size, libmpq__off_t *transferred) {
	/* some common variables. */
	uint32_t i;
	uint32_t blocks                 = 0;
	int32_t result                  = 0;
	libmpq__off_t file_offset       = 0;
	libmpq__off_t unpacked_size     = 0;
	libmpq__off_t transferred_block = 0;
	libmpq__off_t transferred_total = 0;

	/* check if given file number is not out of range. */
	CHECK_FILE_NUM(file_number, mpq_archive)

	/* get target size of block. */
	libmpq__file_size_unpacked(mpq_archive, file_number, &unpacked_size);

	/* check if target buffer is to small. */
	if (unpacked_size > out_size) {

		/* output buffer size is to small or block size is unknown. */
		return LIBMPQ_ERROR_SIZE;
	}

	/* fetch file offset. */
	libmpq__file_offset(mpq_archive, file_number, &file_offset);

	/* get block count for file. */
	libmpq__file_blocks(mpq_archive, file_number, &blocks);

	/* open the packed block offset table. */
	if ((result = libmpq__block_open_offset(mpq_archive, file_number)) < 0) {

		/* something on opening packed block offset table failed. */
		return result;
	}

	/* loop through all blocks. */
	for (i = 0; i < blocks; i++) {

		/* cleanup size variable. */
		unpacked_size = 0;

		/* get unpacked block size. */
		libmpq__block_size_unpacked(mpq_archive, file_number, i, &unpacked_size);

		/* read block. */
		if ((result = libmpq__block_read_with_temporary_buffer(mpq_archive, file_number, i, out_buf + transferred_total, unpacked_size, tmp_buf, tmp_size, &transferred_block)) < 0) {

			/* close the packed block offset table. */
			libmpq__block_close_offset(mpq_archive, file_number);

			/* something on reading block failed. */
			return result;
		}

		transferred_total += transferred_block;

	}

	/* close the packed block offset table. */
	libmpq__block_close_offset(mpq_archive, file_number);

	/* check for null pointer. */
	if (transferred != NULL) {

		/* store transferred bytes. */
		*transferred = transferred_total;
	}

	/* if no error was found, return zero. */
	return LIBMPQ_SUCCESS;
}

/* this function read the given file from archive into a buffer. calculates the filename-based decryption key if needed. */
int32_t libmpq__file_read_with_filename_and_temporary_buffer(mpq_archive_s *mpq_archive, uint32_t file_number, const char *filename, uint8_t *out_buf, libmpq__off_t out_size, uint8_t *tmp_buf, libmpq__off_t tmp_size, libmpq__off_t *transferred) {
	int32_t result = 0;

	if ((result = libmpq__block_open_offset_with_filename(mpq_archive, file_number, filename)) < 0) {
		return result;
	}

	result = libmpq__file_read_with_temporary_buffer(mpq_archive, file_number, out_buf, out_size, tmp_buf, tmp_size, transferred);

	libmpq__block_close_offset(mpq_archive, file_number);

	return result;
}

/* opens a file and calculates the filename-based decryption key if needed. */
int32_t libmpq__block_open_offset_with_filename(mpq_archive_s *mpq_archive, uint32_t file_number, const char *filename) {

	/* some common variables. */
	uint32_t i;
	uint32_t packed_size;
	int32_t result = 0;

	/* check if given file number is not out of range. */
	CHECK_FILE_NUM(file_number, mpq_archive)

	if (mpq_archive->mpq_file[file_number]) {

		/* file already opened, so increment counter */
		mpq_archive->mpq_file[file_number]->open_count++;
		return LIBMPQ_SUCCESS;
	}

	mpq_block_s *mpq_block = &mpq_archive->mpq_block[mpq_archive->mpq_map[file_number].block_table_indices];

	/* check if file is not stored in a single sector. */
	if ((mpq_block->flags & LIBMPQ_FLAG_SINGLE) == 0) {

		/* get packed size based on block size and block count. */
		packed_size = sizeof(uint32_t) * (((mpq_block->unpacked_size + mpq_archive->block_size - 1) / mpq_archive->block_size) + 1);
	} else {

		/* file is stored in single sector and we need only two entries for the packed block offset table. */
		packed_size = sizeof(uint32_t) * 2;
	}

	/* check if data has one extra entry. */
	if ((mpq_block->flags & LIBMPQ_FLAG_CRC) != 0) {

		/* add one uint32_t. */
		packed_size += sizeof(uint32_t);
	}

	/* allocate memory for the file. */
	if ((mpq_archive->mpq_file[file_number] = calloc(1, sizeof(mpq_file_s))) == NULL) {

		/* memory allocation problem. */
		result = LIBMPQ_ERROR_MALLOC;
		goto error;
	}

	/* allocate memory for the packed block offset table. */
	if ((mpq_archive->mpq_file[file_number]->packed_offset = calloc(1, packed_size)) == NULL) {

		/* memory allocation problem. */
		result = LIBMPQ_ERROR_MALLOC;
		goto error;
	}

	/* initialize counter to one opening */
	mpq_archive->mpq_file[file_number]->open_count = 1;

	/* check if we need to load the packed block offset table, we will maintain this table for unpacked files too. */
	if ((mpq_block->flags & LIBMPQ_FLAG_COMPRESSED) != 0 &&
	    (mpq_block->flags & LIBMPQ_FLAG_SINGLE) == 0) {
		uint32_t read_size;

		/* seek to block position. */
		if (fseeko(mpq_archive->fp, mpq_block->offset + (((long long)mpq_archive->mpq_block_ex[mpq_archive->mpq_map[file_number].block_table_indices].offset_high) << 32) + mpq_archive->archive_offset, SEEK_SET) < 0) {

			/* seek in file failed. */
			result = LIBMPQ_ERROR_SEEK;
			goto error;
		}

		/* read block positions from begin of file. */
		if (fread(mpq_archive->mpq_file[file_number]->packed_offset, 1, packed_size, mpq_archive->fp) != packed_size) {

			/* something on read from archive failed. */
			result = LIBMPQ_ERROR_READ;
			goto error;
		}

		/* check if the archive is protected some way, sometimes the file appears not to be encrypted, but it is.
		 * a special case are files with an additional sector but LIBMPQ_FLAG_CRC not set. we don't want to handle
		 * them as encrypted. before using size loaded from file we need to perform conversion to platform endian. */
		read_size = libmpq__bswap_LE32(mpq_archive->mpq_file[file_number]->packed_offset[0]);
		if (read_size != packed_size && read_size != packed_size + 4) {
			/* file is encrypted. */
			mpq_block->flags |= LIBMPQ_FLAG_ENCRYPTED;
		}

		/* check if packed offset block is encrypted, we have to decrypt it. */
		if (mpq_block->flags & LIBMPQ_FLAG_ENCRYPTED) {

			/* get the file seed. */
			uint32_t seed;
			if (mpq_block->flags & LIBMPQ_FLAG_ENCRYPTION_KEY_V2) {
				result = libmpq__encryption_key_from_filename_v2(filename, mpq_block->offset, mpq_block->unpacked_size, &seed);
			} else {
				result = libmpq__encryption_key_from_filename(filename, &seed);
			}
			if (result < 0) {
				result = LIBMPQ_ERROR_DECRYPT;
				goto error;
			}
			mpq_archive->mpq_file[file_number]->seed = seed;

			/* convert to platform endian before decryption */
			bswap_uint32s(mpq_archive->mpq_file[file_number]->packed_offset, packed_size / sizeof(uint32_t));

			/* decrypt block in input buffer. */
			if (libmpq__decrypt_block(mpq_archive->mpq_file[file_number]->packed_offset, packed_size, seed - 1) < 0 ) {

				/* something on decrypt failed. */
				result = LIBMPQ_ERROR_DECRYPT;
				goto error;
			}

			/*
			 * we loaded an array of bytes, so no further endian conversion is needed.
			 */

			/* check if the block positions are correctly decrypted. */
			if (mpq_archive->mpq_file[file_number]->packed_offset[0] != packed_size) {

				/* sorry without seed, we cannot extract file. */
				result = LIBMPQ_ERROR_DECRYPT;
				goto error;
			}
		} else {
			/* convert offsets to platform endian */
			bswap_uint32s(mpq_archive->mpq_file[file_number]->packed_offset, packed_size / sizeof(uint32_t));
		}
	} else {

		/* check if file is not stored in a single sector. */
		if ((mpq_block->flags & LIBMPQ_FLAG_SINGLE) == 0) {

			/* loop through all blocks and create packed block offset table based on block size. */
			for (i = 0; i < ((mpq_block->unpacked_size + mpq_archive->block_size - 1) / mpq_archive->block_size + 1); i++) {

				/* check if we process the last block. */
				if (i == ((mpq_block->unpacked_size + mpq_archive->block_size - 1) / mpq_archive->block_size)) {

					/* store size of last block. */
					mpq_archive->mpq_file[file_number]->packed_offset[i] = mpq_block->unpacked_size;
				} else {

					/* store default block size. */
					mpq_archive->mpq_file[file_number]->packed_offset[i] = i * mpq_archive->block_size;
				}
			}
		} else {

			/* store offsets. */
			mpq_archive->mpq_file[file_number]->packed_offset[0] = 0;
			mpq_archive->mpq_file[file_number]->packed_offset[1] = mpq_block->packed_size;
		}

		if (mpq_block->flags & LIBMPQ_FLAG_ENCRYPTED) {
			/* get the file seed. */
			uint32_t seed;
			if (mpq_block->flags & LIBMPQ_FLAG_ENCRYPTION_KEY_V2) {
				result = libmpq__encryption_key_from_filename_v2(filename, mpq_block->offset, mpq_block->unpacked_size, &seed);
			} else {
				result = libmpq__encryption_key_from_filename(filename, &seed);
			}
			if (result < 0) {
				result = LIBMPQ_ERROR_DECRYPT;
				goto error;
			}
			mpq_archive->mpq_file[file_number]->seed = seed;
		}
	}

	/* if no error was found, return zero. */
	return LIBMPQ_SUCCESS;

error:

	/* free packed block offset table and file pointer. */
	free(mpq_archive->mpq_file[file_number]->packed_offset);
	free(mpq_archive->mpq_file[file_number]);

	/* return error constant. */
	return result;
}

/* this function open a file in the given archive and caches the block offset information. */
int32_t libmpq__block_open_offset(mpq_archive_s *mpq_archive, uint32_t file_number) {

	/* some common variables. */
	uint32_t i;
	uint32_t packed_size;
	int32_t result = 0;

	/* check if given file number is not out of range. */
	CHECK_FILE_NUM(file_number, mpq_archive)

	if (mpq_archive->mpq_file[file_number]) {

		/* file already opened, so increment counter */
		mpq_archive->mpq_file[file_number]->open_count++;
		return LIBMPQ_SUCCESS;
	}

	/* check if file is not stored in a single sector. */
	if ((mpq_archive->mpq_block[mpq_archive->mpq_map[file_number].block_table_indices].flags & LIBMPQ_FLAG_SINGLE) == 0) {

		/* get packed size based on block size and block count. */
		packed_size = sizeof(uint32_t) * (((mpq_archive->mpq_block[mpq_archive->mpq_map[file_number].block_table_indices].unpacked_size + mpq_archive->block_size - 1) / mpq_archive->block_size) + 1);
	} else {

		/* file is stored in single sector and we need only two entries for the packed block offset table. */
		packed_size = sizeof(uint32_t) * 2;
	}

	/* check if data has one extra entry. */
	if ((mpq_archive->mpq_block[mpq_archive->mpq_map[file_number].block_table_indices].flags & LIBMPQ_FLAG_CRC) != 0) {

		/* add one uint32_t. */
		packed_size += sizeof(uint32_t);
	}

	/* allocate memory for the file. */
	if ((mpq_archive->mpq_file[file_number] = calloc(1, sizeof(mpq_file_s))) == NULL) {

		/* memory allocation problem. */
		result = LIBMPQ_ERROR_MALLOC;
		goto error;
	}

	/* allocate memory for the packed block offset table. */
	if ((mpq_archive->mpq_file[file_number]->packed_offset = calloc(1, packed_size)) == NULL) {

		/* memory allocation problem. */
		result = LIBMPQ_ERROR_MALLOC;
		goto error;
	}

	/* initialize counter to one opening */
	mpq_archive->mpq_file[file_number]->open_count = 1;

	/* check if we need to load the packed block offset table, we will maintain this table for unpacked files too. */
	if ((mpq_archive->mpq_block[mpq_archive->mpq_map[file_number].block_table_indices].flags & LIBMPQ_FLAG_COMPRESSED) != 0 &&
	    (mpq_archive->mpq_block[mpq_archive->mpq_map[file_number].block_table_indices].flags & LIBMPQ_FLAG_SINGLE) == 0) {
		uint32_t read_size;

		/* seek to block position. */
		if (fseeko(mpq_archive->fp, mpq_archive->mpq_block[mpq_archive->mpq_map[file_number].block_table_indices].offset + (((long long)mpq_archive->mpq_block_ex[mpq_archive->mpq_map[file_number].block_table_indices].offset_high) << 32) + mpq_archive->archive_offset, SEEK_SET) < 0) {

			/* seek in file failed. */
			result = LIBMPQ_ERROR_SEEK;
			goto error;
		}

		/* read block positions from begin of file. */
		if (fread(mpq_archive->mpq_file[file_number]->packed_offset, 1, packed_size, mpq_archive->fp) != packed_size) {

			/* something on read from archive failed. */
			result = LIBMPQ_ERROR_READ;
			goto error;
		}

		/* check if the archive is protected some way, sometimes the file appears not to be encrypted, but it is.
		 * a special case are files with an additional sector but LIBMPQ_FLAG_CRC not set. we don't want to handle
		 * them as encrypted. before using size loaded from file we need to perform conversion to platform endian. */
		read_size = libmpq__bswap_LE32(mpq_archive->mpq_file[file_number]->packed_offset[0]);
		if (read_size != packed_size && read_size != packed_size + 4) {

			/* file is encrypted. */
			mpq_archive->mpq_block[mpq_archive->mpq_map[file_number].block_table_indices].flags |= LIBMPQ_FLAG_ENCRYPTED;
		}

		/* check if packed offset block is encrypted, we have to decrypt it. */
		if (mpq_archive->mpq_block[mpq_archive->mpq_map[file_number].block_table_indices].flags & LIBMPQ_FLAG_ENCRYPTED) {

			/* check if we don't know the file seed, try to find it. */
			uint32_t seed;
			if (libmpq__decrypt_key((uint8_t *)mpq_archive->mpq_file[file_number]->packed_offset, packed_size, mpq_archive->block_size, &seed) < 0) {

				/* sorry without seed, we cannot extract file. */
				result = LIBMPQ_ERROR_DECRYPT;
				goto error;
			}
			mpq_archive->mpq_file[file_number]->seed = seed;

			/* convert data to platform endian before decryption */
			bswap_uint32s(mpq_archive->mpq_file[file_number]->packed_offset, packed_size / sizeof(uint32_t));

			/* decrypt block in input buffer. */
			if (libmpq__decrypt_block(mpq_archive->mpq_file[file_number]->packed_offset, packed_size, mpq_archive->mpq_file[file_number]->seed - 1) < 0 ) {

				/* something on decrypt failed. */
				result = LIBMPQ_ERROR_DECRYPT;
				goto error;
			}

			/*
			 * we loaded an array of bytes, so no further endian conversion is needed.
			 */

			/* check if the block positions are correctly decrypted. */
			if (mpq_archive->mpq_file[file_number]->packed_offset[0] != packed_size) {

				/* sorry without seed, we cannot extract file. */
				result = LIBMPQ_ERROR_DECRYPT;
				goto error;
			}
		} else {
			/* convert offsets to platform endian */
			bswap_uint32s(mpq_archive->mpq_file[file_number]->packed_offset, packed_size / sizeof(uint32_t));
		}
	} else {

		/* check if file is not stored in a single sector. */
		if ((mpq_archive->mpq_block[mpq_archive->mpq_map[file_number].block_table_indices].flags & LIBMPQ_FLAG_SINGLE) == 0) {

			/* loop through all blocks and create packed block offset table based on block size. */
			for (i = 0; i < ((mpq_archive->mpq_block[mpq_archive->mpq_map[file_number].block_table_indices].unpacked_size + mpq_archive->block_size - 1) / mpq_archive->block_size + 1); i++) {

				/* check if we process the last block. */
				if (i == ((mpq_archive->mpq_block[mpq_archive->mpq_map[file_number].block_table_indices].unpacked_size + mpq_archive->block_size - 1) / mpq_archive->block_size)) {

					/* store size of last block. */
					mpq_archive->mpq_file[file_number]->packed_offset[i] = mpq_archive->mpq_block[mpq_archive->mpq_map[file_number].block_table_indices].unpacked_size;
				} else {

					/* store default block size. */
					mpq_archive->mpq_file[file_number]->packed_offset[i] = i * mpq_archive->block_size;
				}
			}
		} else {

			/* store offsets. */
			mpq_archive->mpq_file[file_number]->packed_offset[0] = 0;
			mpq_archive->mpq_file[file_number]->packed_offset[1] = mpq_archive->mpq_block[mpq_archive->mpq_map[file_number].block_table_indices].packed_size;
		}
	}

	/* if no error was found, return zero. */
	return LIBMPQ_SUCCESS;

error:

	/* free packed block offset table and file pointer. */
	free(mpq_archive->mpq_file[file_number]->packed_offset);
	free(mpq_archive->mpq_file[file_number]);

	/* return error constant. */
	return result;
}

/* this function free the file pointer to the opened file in archive. */
int32_t libmpq__block_close_offset(mpq_archive_s *mpq_archive, uint32_t file_number) {

	/* check if given file number is not out of range. */
	CHECK_FILE_NUM(file_number, mpq_archive)

	if (mpq_archive->mpq_file[file_number] == NULL) {

		/* packed block offset table is not opened. */
		return LIBMPQ_ERROR_OPEN;
	}

	mpq_archive->mpq_file[file_number]->open_count--;

	if (mpq_archive->mpq_file[file_number]->open_count != 0) {

		/* still in use */
		return LIBMPQ_SUCCESS;
	}

	/* free packed block offset table and file pointer. */
	free(mpq_archive->mpq_file[file_number]->packed_offset);
	free(mpq_archive->mpq_file[file_number]);

	/* mark it as unopened - libmpq__block_open_offset checks for this to decide whether to increment the counter */
	mpq_archive->mpq_file[file_number] = NULL;

	/* if no error was found, return zero. */
	return LIBMPQ_SUCCESS;
}

/* this function return the unpacked size of the given file and block in the archive. */
int32_t libmpq__block_size_unpacked(mpq_archive_s *mpq_archive, uint32_t file_number, uint32_t block_number, libmpq__off_t *unpacked_size) {

	/* check if given file number is not out of range. */
	CHECK_FILE_NUM(file_number, mpq_archive)

	/* check if given block number is not out of range. */
	CHECK_BLOCK_NUM(block_number, mpq_archive)

	/* check if packed block offset table is opened. */
	if (mpq_archive->mpq_file[file_number] == NULL ||
	    mpq_archive->mpq_file[file_number]->packed_offset == NULL) {

		/* packed block offset table is not opened. */
		return LIBMPQ_ERROR_OPEN;
	}

	/* check if block is stored as single sector. */
	if ((mpq_archive->mpq_block[mpq_archive->mpq_map[file_number].block_table_indices].flags & LIBMPQ_FLAG_SINGLE) != 0) {

		/* return the unpacked size of the block in the mpq archive. */
		*unpacked_size = mpq_archive->mpq_block[mpq_archive->mpq_map[file_number].block_table_indices].unpacked_size;
	}

	/* check if block is not stored as single sector. */
	if ((mpq_archive->mpq_block[mpq_archive->mpq_map[file_number].block_table_indices].flags & LIBMPQ_FLAG_SINGLE) == 0) {

		/* check if we not process the last block. */
		if (block_number < ((mpq_archive->mpq_block[mpq_archive->mpq_map[file_number].block_table_indices].unpacked_size + mpq_archive->block_size - 1) / mpq_archive->block_size) - 1) {

			/* return the block size as unpacked size. */
			*unpacked_size = mpq_archive->block_size;
		} else {

			/* return the unpacked size of the last block in the mpq archive. */
			*unpacked_size = mpq_archive->mpq_block[mpq_archive->mpq_map[file_number].block_table_indices].unpacked_size - mpq_archive->block_size * block_number;
		}
	}

	/* if no error was found, return zero. */
	return LIBMPQ_SUCCESS;
}

/* this function return the decryption seed for the given file and block. */
int32_t libmpq__block_seed(mpq_archive_s *mpq_archive, uint32_t file_number, uint32_t block_number, uint32_t *seed) {

	/* check if given file number is not out of range. */
	CHECK_FILE_NUM(file_number, mpq_archive)

	/* check if given block number is not out of range. */
	CHECK_BLOCK_NUM(block_number, mpq_archive)

	/* check if packed block offset table is opened. */
	if (mpq_archive->mpq_file[file_number] == NULL ||
	    mpq_archive->mpq_file[file_number]->packed_offset == NULL) {

		/* packed block offset table is not opened. */
		return LIBMPQ_ERROR_OPEN;
	}

	/* return the decryption key. */
	*seed = mpq_archive->mpq_file[file_number]->seed + block_number;

	/* if no error was found, return zero. */
	return LIBMPQ_SUCCESS;
}

/* this function read the given block from archive into a buffer. */
int32_t libmpq__block_read(mpq_archive_s *mpq_archive, uint32_t file_number, uint32_t block_number, uint8_t *out_buf, libmpq__off_t out_size, libmpq__off_t *transferred) {

	/* some common variables. */
	uint8_t *in_buf;
	uint32_t seed               = 0;
	uint32_t encrypted          = 0;
	uint32_t compressed         = 0;
	uint32_t imploded           = 0;
	int32_t tb                  = 0;
	libmpq__off_t block_offset  = 0;
	libmpq__off_t in_size       = 0;
	libmpq__off_t unpacked_size = 0;

	/* check if given file number is not out of range. */
	CHECK_FILE_NUM(file_number, mpq_archive)

	/* check if given block number is not out of range. */
	CHECK_BLOCK_NUM(block_number, mpq_archive)

	/* check if packed block offset table is opened. */
	if (mpq_archive->mpq_file[file_number] == NULL ||
	    mpq_archive->mpq_file[file_number]->packed_offset == NULL) {

		/* packed block offset table is not opened. */
		return LIBMPQ_ERROR_OPEN;
	}

	/* get target size of block. */
	libmpq__block_size_unpacked(mpq_archive, file_number, block_number, &unpacked_size);

	/* check if target buffer is to small. */
	if (unpacked_size > out_size) {

		/* output buffer size is to small or block size is unknown. */
		return LIBMPQ_ERROR_SIZE;
	}

	/* fetch some required values like input buffer size and block offset. */
	block_offset = mpq_archive->mpq_block[mpq_archive->mpq_map[file_number].block_table_indices].offset + (((long long)mpq_archive->mpq_block_ex[mpq_archive->mpq_map[file_number].block_table_indices].offset_high) << 32) + mpq_archive->mpq_file[file_number]->packed_offset[block_number];
	in_size = mpq_archive->mpq_file[file_number]->packed_offset[block_number + 1] - mpq_archive->mpq_file[file_number]->packed_offset[block_number];

	/* get implosion status. */
	libmpq__file_imploded(mpq_archive, file_number, &imploded);

	/* get compression status. */
	libmpq__file_compressed(mpq_archive, file_number, &compressed);

	/* check if we can avoid allocating a temporary buffer. */
	const uint8_t use_in_buf_as_out = in_size <= out_size && (!compressed || in_size == out_size) && !imploded;

	/* seek in file. */
	if (fseeko(mpq_archive->fp, block_offset + mpq_archive->archive_offset, SEEK_SET) < 0) {

		/* something with seek in file failed. */
		return LIBMPQ_ERROR_SEEK;
	}

	if (use_in_buf_as_out) {
		in_buf = out_buf;
	} else {
		/* allocate memory for the read buffer. */
		if ((in_buf = calloc(1, in_size)) == NULL) {

			/* memory allocation problem. */
			return LIBMPQ_ERROR_MALLOC;
		}
	}

	/* read block from file. */
	if (fread(in_buf, 1, in_size, mpq_archive->fp) != in_size) {

		/* free buffers. */
		if (!use_in_buf_as_out) free(in_buf);

		/* something on reading block failed. */
		return LIBMPQ_ERROR_READ;
	}

	/* get encryption status. */
	libmpq__file_encrypted(mpq_archive, file_number, &encrypted);

	/* check if file is encrypted. */
	if (encrypted) {

		/* get decryption key. */
		libmpq__block_seed(mpq_archive, file_number, block_number, &seed);

		/* convert data to platform endian before decryption */
		bswap_uint32s((uint32_t *)in_buf, in_size / sizeof(uint32_t));

		/* decrypt block. */
		if (libmpq__decrypt_block((uint32_t *)in_buf, in_size, seed) < 0) {

			/* free buffers. */
			free(in_buf);

			/* something on decrypting block failed. */
			return LIBMPQ_ERROR_DECRYPT;
		}

		/* revert to format endian after decryption */
		bswap_uint32s((uint32_t *)in_buf, in_size / sizeof(uint32_t));
	}

	/* check if file is compressed. */
	if (compressed) {

		/* decompress block. */
		int32_t result;
		uint8_t *work_buf;
		if ((result = get_decompression_work_buf_multi(mpq_archive, &work_buf)) != 0) return result;
		if ((tb = libmpq__decompress_block(in_buf, in_size, work_buf, out_buf, out_size, LIBMPQ_FLAG_COMPRESS_MULTI)) < 0) {

			/* free temporary buffer. */
			if (!use_in_buf_as_out) free(in_buf);

			/* something on decompressing block failed. */
			return LIBMPQ_ERROR_UNPACK;
		}
	}

	/* check if file is imploded. */
	if (imploded) {

		/* explode block. */
		int32_t result;
		uint8_t *work_buf;
		if ((result = get_decompression_work_buf_pkzip(mpq_archive, &work_buf)) != 0) return result;
		if ((tb = libmpq__decompress_block(in_buf, in_size, work_buf, out_buf, out_size, LIBMPQ_FLAG_COMPRESS_PKZIP)) < 0) {

			/* free temporary buffer. */
			free(in_buf);

			/* something on decompressing block failed. */
			return LIBMPQ_ERROR_UNPACK;
		}
	}

	/* files should not be compressed and imploded */
	if (compressed && imploded) {
		/* free temporary buffer. */
		free(in_buf);

		/* something on decompressing block failed. */
		return LIBMPQ_ERROR_UNPACK;
	}

	/* check if file is neither compressed nor imploded. */
	if (!compressed && !imploded) {

		/* copy block. */
		if ((tb = libmpq__decompress_block(in_buf, in_size, NULL, out_buf, out_size, LIBMPQ_FLAG_COMPRESS_NONE)) < 0) {

			/* free temporary buffer. */
			if (!use_in_buf_as_out) free(in_buf);

			/* something on decompressing block failed. */
			return LIBMPQ_ERROR_UNPACK;
		}
	}

	/* free read buffer. */
	if (!use_in_buf_as_out) free(in_buf);

	/* check for null pointer. */
	if (transferred != NULL) {

		/* store transferred bytes. */
		*transferred = tb;
	}

	/* if no error was found, return zero. */
	return LIBMPQ_SUCCESS;
}

/* this function read the given block from archive into a buffer with a user-provided temporary buffer. */
int32_t libmpq__block_read_with_temporary_buffer(mpq_archive_s *mpq_archive, uint32_t file_number, uint32_t block_number, uint8_t *out_buf, libmpq__off_t out_size, uint8_t *tmp_buf, libmpq__off_t tmp_size, libmpq__off_t *transferred) {

	/* some common variables. */
	uint32_t seed               = 0;
	uint32_t encrypted          = 0;
	uint32_t compressed         = 0;
	uint32_t imploded           = 0;
	int32_t tb                  = 0;
	libmpq__off_t block_offset  = 0;
	libmpq__off_t in_size   = 0;
	libmpq__off_t unpacked_size = 0;

	/* check if given file number is not out of range. */
	CHECK_FILE_NUM(file_number, mpq_archive)

	/* check if given block number is not out of range. */
	CHECK_BLOCK_NUM(block_number, mpq_archive)

	/* check if packed block offset table is opened. */
	if (mpq_archive->mpq_file[file_number] == NULL ||
	    mpq_archive->mpq_file[file_number]->packed_offset == NULL) {

		/* packed block offset table is not opened. */
		return LIBMPQ_ERROR_OPEN;
	}

	/* get target size of block. */
	libmpq__block_size_unpacked(mpq_archive, file_number, block_number, &unpacked_size);

	/* check if target buffer is to small. */
	if (unpacked_size > out_size) {

		/* output buffer size is to small or block size is unknown. */
		return LIBMPQ_ERROR_SIZE;
	}

	/* fetch some required values like input buffer size and block offset. */
	block_offset = mpq_archive->mpq_block[mpq_archive->mpq_map[file_number].block_table_indices].offset + (((long long)mpq_archive->mpq_block_ex[mpq_archive->mpq_map[file_number].block_table_indices].offset_high) << 32) + mpq_archive->mpq_file[file_number]->packed_offset[block_number];
	in_size = mpq_archive->mpq_file[file_number]->packed_offset[block_number + 1] - mpq_archive->mpq_file[file_number]->packed_offset[block_number];

	if (tmp_size < in_size) {
		return LIBMPQ_ERROR_SIZE;
	}

	/* check if we can avoid allocating a temporary buffer. */
	const uint8_t use_in_buf_as_out = in_size <= out_size && (!compressed || in_size == out_size) && !imploded;

	/* seek in file. */
	if (fseeko(mpq_archive->fp, block_offset + mpq_archive->archive_offset, SEEK_SET) < 0) {

		/* something with seek in file failed. */
		return LIBMPQ_ERROR_SEEK;
	}

	/* read block from file. */
	if (fread(tmp_buf, 1, in_size, mpq_archive->fp) != in_size) {

		/* something on reading block failed. */
		return LIBMPQ_ERROR_READ;
	}

	/* get encryption status. */
	libmpq__file_encrypted(mpq_archive, file_number, &encrypted);

	/* check if file is encrypted. */
	if (encrypted) {

		/* get decryption key. */
		libmpq__block_seed(mpq_archive, file_number, block_number, &seed);

		/* convert data to platform endian before decryption */
		bswap_uint32s((uint32_t *)tmp_buf, in_size / sizeof(uint32_t));

		/* decrypt block. */
		if (libmpq__decrypt_block((uint32_t *)tmp_buf, in_size, seed) < 0) {

			/* something on decrypting block failed. */
			return LIBMPQ_ERROR_DECRYPT;
		}

		/* revert to format endian after decryption */
		bswap_uint32s((uint32_t *)tmp_buf, in_size / sizeof(uint32_t));
	}

	/* get compression status. */
	libmpq__file_compressed(mpq_archive, file_number, &compressed);

	/* check if file is compressed. */
	if (compressed) {

		/* decompress block. */
		int32_t result;
		uint8_t *work_buf;
		if ((result = get_decompression_work_buf_multi(mpq_archive, &work_buf)) != 0) return result;
		if ((tb = libmpq__decompress_block(tmp_buf, in_size, work_buf, out_buf, out_size, LIBMPQ_FLAG_COMPRESS_MULTI)) < 0) {

			/* something on decompressing block failed. */
			return LIBMPQ_ERROR_UNPACK;
		}
	}

	/* get implosion status. */
	libmpq__file_imploded(mpq_archive, file_number, &imploded);

	/* check if file is imploded. */
	if (imploded) {

		/* explode block. */
		int32_t result;
		uint8_t *work_buf;
		if ((result = get_decompression_work_buf_pkzip(mpq_archive, &work_buf)) != 0) return result;
		if ((tb = libmpq__decompress_block(tmp_buf, in_size, work_buf, out_buf, out_size, LIBMPQ_FLAG_COMPRESS_PKZIP)) < 0) {

			/* something on decompressing block failed. */
			return LIBMPQ_ERROR_UNPACK;
		}
	}

	/* files should not be compressed and imploded */
	if (compressed && imploded) {

		/* something on decompressing block failed. */
		return LIBMPQ_ERROR_UNPACK;
	}

	/* check if file is neither compressed nor imploded. */
	if (!compressed && !imploded) {

		/* copy block. */
		if ((tb = libmpq__decompress_block(tmp_buf, in_size, NULL, out_buf, out_size, LIBMPQ_FLAG_COMPRESS_NONE)) < 0) {

			/* something on decompressing block failed. */
			return LIBMPQ_ERROR_UNPACK;
		}
	}

	/* check for null pointer. */
	if (transferred != NULL) {

		/* store transferred bytes. */
		*transferred = tb;
	}

	/* if no error was found, return zero. */
	return LIBMPQ_SUCCESS;
}
