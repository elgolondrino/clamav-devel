/*
 *  Copyright (C) 2004 Nigel Horne <njh@bandsman.co.uk>
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
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA 02110-1301, USA.
 *
 * Much of this code is based on minitar.c which is in the public domain.
 * Author: Charles G. Waldman (cgw@pgt.com),  Aug 4 1998
 * There are many tar files that this code cannot decode.
 */
static	char	const	rcsid[] = "$Id: untar.c,v 1.32 2006/10/15 11:10:42 njh Exp $";

#if HAVE_CONFIG_H
#include "clamav-config.h"
#endif

#include <stdio.h>
#include <errno.h>
#include <string.h>
#ifdef	HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <sys/stat.h>
#include <fcntl.h>
#ifdef	HAVE_SYS_PARAM_H
#include <sys/param.h>	/* for NAME_MAX */
#endif

#include "clamav.h"
#include "others.h"
#include "untar.h"
#include "mbox.h"
#include "blob.h"

#define BLOCKSIZE 512

#ifndef	O_BINARY
#define	O_BINARY	0
#endif

static int
octal(const char *str)
{
	int ret;

	if(sscanf(str, "%o", (unsigned int *)&ret) != 1)
		return -1;
	return ret;
}

int
cli_untar(const char *dir, int desc, unsigned int posix)
{
	int size = 0;
	int in_block = 0;
	char fullname[NAME_MAX + 1];
	FILE *outfile = NULL;

	cli_dbgmsg("In untar(%s, %d)\n", dir ? dir : "", desc);

	for(;;) {
		char block[BLOCKSIZE];
		const int nread = cli_readn(desc, block, (unsigned int)sizeof(block));

		if(!in_block && nread == 0)
			break;

		if(nread < 0) {
			if(outfile)
				fclose(outfile);
			cli_errmsg("cli_untar: block read error\n");
			return CL_EIO;
		}

		if(!in_block) {
			char type;
			const char *suffix;
			size_t suffixLen = 0;
			int fd, directory, skipEntry = 0;
			char magic[7], name[101], osize[13];

			if(outfile) {
				if(fclose(outfile)) {
					cli_errmsg("cli_untar: cannot close file %s\n",
						fullname);
					return CL_EIO;
				}
				outfile = (FILE*)0;
			}

			if(block[0] == '\0')	/* We're done */
				break;

			/* Notice assumption that BLOCKSIZE > 262 */
			if(posix) {
				strncpy(magic, block+257, 5);
				magic[5] = '\0';
				if(strcmp(magic, "ustar") != 0) {
					cli_dbgmsg("Incorrect magic string '%s' in tar header\n", magic);
					return CL_EFORMAT;
				}
			}

			type = block[156];

			/*
			 * Extra types from djgardner@users.sourceforge.net
			 */
			switch(type) {
				case '0':	/* plain file */
				case '\0':	/* plain file */
				case '7':	/* contiguous file */
					directory = 0;
					break;
				case '1':	/* Link to already archived file */
				case '5':	/* directory */
				case '2':	/* sym link */
				case '3':	/* char device */
				case '4':	/* block device */
				case '6':	/* fifo special */
				case 'V':	/* Volume header */
					directory = 1;
					break;
				case 'K':
				case 'L':
					/* GNU extension - ././@LongLink
					 * Discard the blocks with the extended filename,
					 * the last header will contain parts of it anyway
					 */
					directory = 0;
					skipEntry = 1;
					break;
				default:
					/*cli_errmsg("cli_untar: unknown type flag %c\n", type);
					return CL_EFORMAT;*/
					/*
					 * It isn't really a tar file
					 */
					cli_dbgmsg("cli_untar: unknown type flag %c\n", type);
					/*
					 * We don't know that it's clean at all,
					 * it would be better to have a
					 * CL_CONTINUE return value since it
					 * may be a different format
					 */
					return CL_CLEAN;
			}

			if(directory) {
				in_block = 0;
				continue;
			}

			strncpy(osize, block+124, 12);
			osize[12] = '\0';
			size = octal(osize);
			if(size < 0) {
				cli_errmsg("Invalid size in tar header\n");
				if(outfile)
					fclose(outfile);
				return CL_EFORMAT;
			}
			cli_dbgmsg("cli_untar: size = %d\n", size);

			if(skipEntry) {
				const int nskip = (size % BLOCKSIZE || !size) ? size + BLOCKSIZE - (size % BLOCKSIZE) : size;
				cli_dbgmsg("cli_untar: GNU extension, skipping entry\n");
				lseek(desc, nskip, SEEK_CUR);
				continue;
			}

			strncpy(name, block, 100);
			name[100] = '\0';

			/*
			 * see also fileblobSetFilename()
			 * TODO: check if the suffix needs to be put back
			 */
			sanitiseName(name);
			suffix = strrchr(name, '.');
			if(suffix == NULL)
				suffix = "";
			else {
				suffixLen = strlen(suffix);
				if(suffixLen > 4) {
					/* Found a full stop which isn't a suffix */
					suffix = "";
					suffixLen = 0;
				}
			}
			snprintf(fullname, sizeof(fullname) - 1 - suffixLen, "%s/%.*sXXXXXX", dir,
				(int)(sizeof(fullname) - 9 - suffixLen - strlen(dir)), name);
#if	defined(C_LINUX) || defined(C_BSD) || defined(HAVE_MKSTEMP) || defined(C_SOLARIS) || defined(C_CYGWIN)
			fd = mkstemp(fullname);
#else
			(void)mktemp(fullname);
			fd = open(fullname, O_WRONLY|O_CREAT|O_EXCL|O_TRUNC|O_BINARY, 0600);
#endif

			if(fd < 0) {
				cli_errmsg("Can't create temporary file %s: %s\n", fullname, strerror(errno));
				cli_dbgmsg("%lu %d %d\n", suffixLen, sizeof(fullname), strlen(fullname));
				return CL_ETMPFILE;
			}

			cli_dbgmsg("cli_untar: extracting %s\n", fullname);

			in_block = 1;
			if((outfile = fdopen(fd, "wb")) == NULL) {
				cli_errmsg("cli_untar: cannot create file %s\n",
					fullname);
				close(fd);
				return CL_ETMPFILE;
			}
		} else { /* write or continue writing file contents */
			const int nbytes = size>512? 512:size;
			const int nwritten = (int)fwrite(block, 1, (size_t)nbytes, outfile);

			if(nwritten != nbytes) {
				cli_errmsg("cli_untar: only wrote %d bytes to file %s (out of disc space?)\n",
					nwritten, fullname);
				if(outfile)
					fclose(outfile);
				return CL_EIO;
			}
			size -= nbytes;
		}
		if (size == 0)
			in_block = 0;
	}
	if(outfile)
		return fclose(outfile);

	return 0;
}
