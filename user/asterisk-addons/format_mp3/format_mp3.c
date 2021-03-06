/*
 * Asterisk -- A telephony toolkit for Linux.
 *
 * format_mp3.c
 * Anthony Minessale <anthmct@yahoo.com>
 *
 * Derived from other asterisk sound formats by
 * Mark Spencer <markster@linux-support.net>
 *
 * Thanks to mpglib from http://www.mpg123.org/
 * and Chris Stenton [jacs@gnome.co.uk]
 * for coding the ability to play stereo and non-8khz files
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License
 */

#include <asterisk.h>

#include "mpg123.h" 
#include "mpglib.h" 
#include <asterisk/lock.h>
#include <asterisk/channel.h>
#include <asterisk/file.h>
#include <asterisk/logger.h>
#include <asterisk/sched.h>
#include <asterisk/module.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <sys/time.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#ifdef __linux__
#include <endian.h>
#else
#include <machine/endian.h>
#endif

#define AST_MODULE "format_mp3"

#define MP3_BUFLEN 320
#define MP3_SCACHE 16384
#define MP3_DCACHE 8192

/* Based on format_wav.c */

struct mp3_private {
	char waste[AST_FRIENDLY_OFFSET];	/* Buffer for sending frames, etc */
	char empty;				/* Empty character */
	int lasttimeout;
	int maxlen;
	struct timeval last;
	struct mpstr mp;
	char sbuf[MP3_SCACHE];
	char dbuf[MP3_DCACHE];
	int buflen;
	int sbuflen;
	int dbuflen;
	int dbufoffset;
	int sbufoffset;
	int lastseek;
	int offset;
	long seek;
};

static char *name = "mp3";

#define BLOCKSIZE 160
#define OUTSCALE 4096

#define GAIN -4		/* 2^GAIN is the multiple to increase the volume by */

#if __BYTE_ORDER == __LITTLE_ENDIAN
#define htoll(b) (b)
#define htols(b) (b)
#define ltohl(b) (b)
#define ltohs(b) (b)
#else
#if __BYTE_ORDER == __BIG_ENDIAN
#define htoll(b)  \
          (((((b)      ) & 0xFF) << 24) | \
	       ((((b) >>  8) & 0xFF) << 16) | \
		   ((((b) >> 16) & 0xFF) <<  8) | \
		   ((((b) >> 24) & 0xFF)      ))
#define htols(b) \
          (((((b)      ) & 0xFF) << 8) | \
		   ((((b) >> 8) & 0xFF)      ))
#define ltohl(b) htoll(b)
#define ltohs(b) htols(b)
#else
#error "Endianess not defined"
#endif
#endif


static int mp3_open(struct ast_filestream *s)
{
	struct mp3_private *p = s->private;
	
	InitMP3(&p->mp, OUTSCALE);
	p->dbuflen = 0;
	s->fr.data = s->buf;
	s->fr.frametype = AST_FRAME_VOICE;
	s->fr.subclass = AST_FORMAT_SLINEAR;
	/* datalen will vary for each frame */
	s->fr.src = name;
	s->fr.mallocd = 0;
	p->offset = 0;
	return 0;
}


static void mp3_close(struct ast_filestream *s)
{
	struct mp3_private *p = s->private;
	
	ExitMP3(&p->mp);
	return;
}

static int mp3_squeue(struct ast_filestream *s) 
{
	struct mp3_private *p = s->private;
	int res=0;
	
	p->lastseek = ftell(s->f);
	p->sbuflen = fread(p->sbuf, 1, MP3_SCACHE, s->f);
	if(p->sbuflen < 0) {
		ast_log(LOG_WARNING, "Short read (%d) (%s)!\n", p->sbuflen, strerror(errno));
		return -1;
	}
	res = decodeMP3(&p->mp,p->sbuf,p->sbuflen,p->dbuf,MP3_DCACHE,&p->dbuflen);
	if(res != MP3_OK)
		return -1;
	p->sbuflen -= p->dbuflen;
	p->dbufoffset = 0;
	return 0;
}

static int mp3_dqueue(struct ast_filestream *s) 
{
	struct mp3_private *p = s->private;
	int res=0;
	
	if((res = decodeMP3(&p->mp,NULL,0,p->dbuf,MP3_DCACHE,&p->dbuflen)) == MP3_OK) {
		p->sbuflen -= p->dbuflen;
		p->dbufoffset = 0;
	}
	return res;
}

static int mp3_queue(struct ast_filestream *s)
{
	struct mp3_private *p = s->private;
	int res = 0, bytes = 0;
	
	if(p->seek) {
		ExitMP3(&p->mp);
		InitMP3(&p->mp, OUTSCALE);
		fseek(s->f, 0, SEEK_SET);
		p->sbuflen = p->dbuflen = p->offset = 0;
		while(p->offset < p->seek) {
			if(mp3_squeue(s))
				return -1;
			while(p->offset < p->seek && ((res = mp3_dqueue(s))) == MP3_OK) {
				for(bytes = 0 ; bytes < p->dbuflen ; bytes++) {
					p->dbufoffset++;
					p->offset++;
					if(p->offset >= p->seek)
						break;
				}
			}
			if(res == MP3_ERR)
				return -1;
		}
		
		p->seek = 0;
		return 0;
	}
	if(p->dbuflen == 0) {
		if(p->sbuflen) {
			res = mp3_dqueue(s);
			if(res == MP3_ERR)
				return -1;
		}
		if(! p->sbuflen || res != MP3_OK) {
			if(mp3_squeue(s))
				return -1;
		}
		
	}

	return 0;
}

static struct ast_frame *mp3_read(struct ast_filestream *s, int *whennext)
{

	struct mp3_private *p = s->private;
	int delay =0;
	int save=0;

	/* Send a frame from the file to the appropriate channel */

	if(mp3_queue(s))
		return NULL;

	if(p->dbuflen) {
		for(p->buflen=0; p->buflen < MP3_BUFLEN && p->buflen < p->dbuflen; p->buflen++) {
			s->buf[p->buflen] = p->dbuf[p->buflen+p->dbufoffset];
			p->sbufoffset++;
		}
		p->dbufoffset += p->buflen;
		p->dbuflen -= p->buflen;

		if(p->buflen < MP3_BUFLEN) {
			if(mp3_queue(s))
				return NULL;

			for(save = p->buflen; p->buflen < MP3_BUFLEN; p->buflen++) {
				s->buf[p->buflen] = p->dbuf[(p->buflen-save)+p->dbufoffset];
				p->sbufoffset++;
			}
			p->dbufoffset += (MP3_BUFLEN - save);
			p->dbuflen -= (MP3_BUFLEN - save);

		} 

	}
	
	p->offset += p->buflen;
	delay = p->buflen/2;
	s->fr.frametype = AST_FRAME_VOICE;
	s->fr.subclass = AST_FORMAT_SLINEAR;
	s->fr.offset = AST_FRIENDLY_OFFSET;
	s->fr.datalen = p->buflen;
	s->fr.data = s->buf;
	s->fr.mallocd = 0;
	s->fr.samples = delay;
	*whennext = delay;
	return &s->fr;
}


static int mp3_write(struct ast_filestream *fs, struct ast_frame *f)
{
	ast_log(LOG_ERROR,"I Can't write MP3 only read them.\n");
	return -1;

}


static int mp3_seek(struct ast_filestream *s, off_t sample_offset, int whence)
{
	struct mp3_private *p = s->private;
	off_t min,max,cur;
	long offset=0,samples;
	samples = sample_offset * 2;

	min = 0;
	fseek(s->f, 0, SEEK_END);
	max = ftell(s->f) * 100;
	cur = p->offset;

	if (whence == SEEK_SET)
		offset = samples + min;
	else if (whence == SEEK_CUR || whence == SEEK_FORCECUR)
		offset = samples + cur;
	else if (whence == SEEK_END)
		offset = max - samples;
	if (whence != SEEK_FORCECUR) {
		offset = (offset > max)?max:offset;
	}

	p->seek = offset;
	return p->seek;
	
}

static int mp3_rewrite(struct ast_filestream *s, const char *comment) 
{
	ast_log(LOG_ERROR,"I Can't write MP3 only read them.\n");
	return -1;
}

static int mp3_trunc(struct ast_filestream *s) 
{

	ast_log(LOG_ERROR,"I Can't write MP3 only read them.\n");
	return -1;
}

static off_t mp3_tell(struct ast_filestream *s)
{
	struct mp3_private *p = s->private;
	
	return p->offset/2;
}

static char *mp3_getcomment(struct ast_filestream *s)
{
	return NULL;
}

static const struct ast_format mp3_f = {
	.name = "mp3",
	.exts = "mp3",
	.format = AST_FORMAT_SLINEAR,
	.open = mp3_open,
	.write = mp3_write,
	.rewrite = mp3_rewrite,
	.seek =	mp3_seek,
	.trunc = mp3_trunc,
	.tell =	mp3_tell,
	.read =	mp3_read,
	.close = mp3_close,
	.getcomment = mp3_getcomment,
	.buf_size = MP3_BUFLEN + AST_FRIENDLY_OFFSET,
	.desc_size = sizeof(struct mp3_private),
};


static int load_module(void)
{
	InitMP3Constants();
	return ast_format_register(&mp3_f);
}

static int unload_module(void)
{
	return ast_format_unregister(name);
}	

AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY, "MP3 format [Any rate but 8000hz mono is optimal]");
