/*
 * Copyright (c) 2011, 2012 Conformal Systems LLC <info@conformal.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/stat.h>

#include <inttypes.h>
#include <unistd.h>
#include <limits.h>
#include <stdlib.h>
#include <dirent.h>
#include <libgen.h>
#include <fcntl.h>
#include <errno.h>

#ifdef NEED_LIBCLENS
#include <clens.h>
#endif

#include <assl.h>
#include <clog.h>
#include <exude.h>

#include <ctutil.h>

#include <ct_types.h>
#include <ct_crypto.h>
#include <ct_proto.h>
#include <ct_match.h>
#include <ct_ctfile.h>
#include <cyphertite.h>
#include <ct_ext.h>
#include <ct_internal.h>

ct_op_cb		ct_cull_send_shas;
ct_op_cb		ct_cull_setup;
ct_op_cb		ct_cull_start_shas;
ct_op_cb		ct_cull_start_complete;
ct_op_cb		ct_cull_send_complete;
ct_op_complete_cb	ct_cull_complete;
ct_op_cb		ct_cull_collect_ctfiles;
ct_op_complete_cb	ct_cull_fetch_all_ctfiles;

void	 ct_xml_file_open(struct ct_global_state *, struct ct_trans *,
	     const char *, int, uint32_t, ct_complete_fn);

char		*all_ctfiles_pattern[] = {
			"^[[:digit:]]{8}-[[:digit:]]{6}-.*",
			NULL,
		 };
/*
 * clean up after a ctfile archive/extract operation by freeing the remotename
 */
int
ctfile_op_cleanup(struct ct_global_state *state, struct ct_op *op)
{
	struct ct_ctfileop_args	*cca = op->op_args;

	e_free(&cca->cca_remotename);

	return (0);
}

int
ctfile_complete_noop(struct ct_global_state *state, struct ct_trans *trans)
{
	return (0);
}

int
ctfile_complete_noop_final(struct ct_global_state *state,
    struct ct_trans *trans)
{
	return (1);
}

int
ctfile_xml_open_complete(struct ct_global_state *state,
    struct ct_trans *trans)
{
	/* change state and wake up process waiting on us */
	ct_set_file_state(state, CT_S_RUNNING);
	ct_wakeup_file(state->event_state);

	return (0);
}

void
ctfile_archive_free_fnode(struct ct_global_state *state,
    struct ct_trans *trans)
{
	ct_free_fnode(trans->tr_fl_node);
	trans->tr_fl_node = NULL;
}

struct ctfile_archive_state {
	FILE		*cas_handle;
	struct fnode	*cas_fnode;
	off_t		 cas_size;
	off_t		 cas_offset;
	int		 cas_block_no;
	int		 cas_open_sent;
};

void
ctfile_archive(struct ct_global_state *state, struct ct_op *op)
{
	char				 tpath[PATH_MAX];
	struct ct_ctfileop_args		*cca = op->op_args;
	struct ctfile_archive_state	*cas = op->op_priv;
	const char			*ctfile = cca->cca_localname;
	const char			*rname = cca->cca_remotename;
	struct ct_trans			*ct_trans;
	struct stat			 sb;
	ssize_t				 rsz, rlen;
	int				 error;

	if (state->ct_dying)
		goto dying;

	switch (ct_get_file_state(state)) {
	case CT_S_STARTING:
		cas = e_calloc(1, sizeof(*cas));
		op->op_priv = cas;

		if (cca->cca_tdir) {
			snprintf(tpath, sizeof tpath, "%s%c%s",
			    cca->cca_tdir, CT_PATHSEP, ctfile);
		} else {
			strlcpy(tpath, ctfile, sizeof(tpath));
		}
		CNDBG(CT_LOG_FILE, "opening ctfile for archive %s", ctfile);
		if ((cas->cas_handle = ct_fopen(tpath, "rb")) == NULL) {
			ct_fatal(state, ctfile, CTE_ERRNO);
			goto dying;
		}
		if (cca->cca_ctfile) {
			struct ctfile_parse_state	 xs_ctx;
			int				 ret, s_errno;
			if ((error = ctfile_parse_init_f(&xs_ctx,
			    cas->cas_handle, NULL)) != 0) {
				ct_fatal(state, tpath, error);
				goto dying;
			}
			while ((ret = ctfile_parse(&xs_ctx)) != XS_RET_EOF) {
				if (ret == XS_RET_SHA)  {
					if (ctfile_parse_seek(&xs_ctx)) {
						s_errno = errno;
						ret = xs_ctx.xs_errno;
						ctfile_parse_close(&xs_ctx);

						errno = s_errno;
						ct_fatal(state,
						    "Can't seek in ctfile",
						    ret);
						goto dying;
					}
				} else if (ret == XS_RET_FAIL) {
					s_errno = errno;
					ret = xs_ctx.xs_errno;
					ctfile_parse_close(&xs_ctx);

					errno = s_errno;
					/* XXX include name */
					ct_fatal(state,
					    "Not a valid ctfile",
					    ret);
					goto dying;
				}

			}
			ctfile_parse_close(&xs_ctx);
			fseek(cas->cas_handle, 0L, SEEK_SET);
		}

		if (fstat(fileno(cas->cas_handle), &sb) == -1) {
			ct_fatal(state, ctfile, CTE_ERRNO);
			goto dying;
		}
		cas->cas_size = sb.st_size;
		cas->cas_fnode = ct_alloc_fnode();

		if (rname == NULL) {
			if ((rname = ctfile_cook_name(ctfile)) == NULL) {
				ct_fatal(state, ctfile,
				    CTE_INVALID_CTFILE_NAME);
				goto dying;
			}
			cca->cca_remotename = (char *)rname;
		}
		break;
	case CT_S_FINISHED:
		/* We're done here */
		return;
	case CT_S_WAITING_SERVER:
		CNDBG(CT_LOG_FILE, "waiting on remote open");
		return;
	default:
		break;
	}

	CNDBG(CT_LOG_FILE, "entered for block %d", cas->cas_block_no);
	ct_set_file_state(state, CT_S_RUNNING);
loop:
	ct_trans = ct_trans_alloc(state);
	if (ct_trans == NULL) {
		/* system busy, return */
		CNDBG(CT_LOG_TRANS, "ran out of transactions, waiting");
		ct_set_file_state(state, CT_S_WAITING_TRANS);
		return;
	}
	ct_trans->tr_statemachine = ct_state_ctfile_archive;

	if (cas->cas_open_sent == 0) {
		cas->cas_open_sent = 1;
		ct_xml_file_open(state, ct_trans, rname, MD_O_WRITE, 0,
		    ctfile_xml_open_complete);
		/* xml thread will wake us up when it gets the open */
		ct_set_file_state(state, CT_S_WAITING_SERVER);
		return;
	}

	/* Are we done here? */
	if (cas->cas_size == cas->cas_offset) {
		ct_trans->tr_fl_node = NULL;
		ct_trans->tr_state = TR_S_XML_CLOSE;
		ct_trans->tr_complete = ctfile_complete_noop_final;
		ct_trans->tr_cleanup = NULL;
		ct_trans->tr_eof = 1;
		ct_trans->hdr.c_flags = C_HDR_F_METADATA;
		ct_trans->tr_ctfile_name = rname;
		state->ct_stats->st_bytes_tot += cas->cas_size;
		(void)fclose(cas->cas_handle);
		e_free(&cas);
		op->op_priv = NULL;
		ct_queue_first(state, ct_trans);
		ct_set_file_state(state, CT_S_FINISHED);
		CNDBG(CT_LOG_FILE, "setting eof on trans %" PRIu64,
		    ct_trans->tr_trans_id);
		return;
	}
	/* perform read */
	rsz = cas->cas_size - cas->cas_offset;

	CNDBG(CT_LOG_FILE, "rsz %ld max %d", (long) rsz,
	    state->ct_max_block_size);
	if (rsz > state->ct_max_block_size) {
		rsz = state->ct_max_block_size;
	}

	ct_trans->tr_dataslot = 0;
	rlen = fread(ct_trans->tr_data[0], sizeof(char), rsz, cas->cas_handle);

	CNDBG(CT_LOG_FILE, "read %ld", (long) rlen);

	state->ct_stats->st_bytes_read += rlen;

	ct_ref_fnode(cas->cas_fnode);
	ct_trans->tr_fl_node = cas->cas_fnode;
	ct_trans->tr_chsize = ct_trans->tr_size[0] = rlen;
	ct_trans->tr_state = TR_S_READ;
	/* nothing to do when the data is on the server */
	ct_trans->tr_complete = ctfile_complete_noop;
	ct_trans->tr_cleanup = ctfile_archive_free_fnode;
	ct_trans->tr_type = TR_T_WRITE_CHUNK;
	ct_trans->tr_eof = 0;
	ct_trans->hdr.c_flags = C_HDR_F_METADATA;
	ct_trans->hdr.c_flags |= ((cca->cca_cleartext == 0) ?
	    C_HDR_F_ENCRYPTED : 0);
	ct_trans->hdr.c_ex_status = 2; /* we handle new metadata protocol */
	/* Set chunkno for restart and for iv generation */
	ct_trans->tr_ctfile_chunkno = cas->cas_block_no;
	ct_trans->tr_ctfile_name = rname;

	cas->cas_block_no++;

	if (rsz != rlen || (rlen + cas->cas_offset) == cas->cas_size) {
		/* short read, file truncated or EOF */
		CNDBG(CT_LOG_FILE, "DONE");
		error = fstat(fileno(cas->cas_handle), &sb);
		if (error) {
			CWARNX("file stat error %s %d %s",
			    ctfile, errno, strerror(errno));
		} else if (sb.st_size != cas->cas_size) {
			CWARNX("file truncated during backup %s",
			    ctfile);
			/*
			 * may need to perform special nop processing
			 * to pad archive file to right number of chunks
			 */
		}
		/*
		 * we don't set eof here because the next go round
		 * will hit the state done case above
		 */
		cas->cas_offset = cas->cas_size;
		ct_trans->tr_eof = 1;

		/* done with fnode, release ref */
		ct_free_fnode(cas->cas_fnode);
		cas->cas_fnode = NULL;

	} else {
		cas->cas_offset += rlen;
	}
	ct_queue_first(state, ct_trans);
	CNDBG(CT_LOG_FILE, " trans %"PRId64", read size %ld, into %p rlen %ld",
	    ct_trans->tr_trans_id, (long) rsz, ct_trans->tr_data[0],
	    (long) rlen);
	CNDBG(CT_LOG_FILE, "sizes rlen %ld offset %ld size %ld", (long) rlen,
	    (long)cas->cas_offset, (long)cas->cas_size);


	goto loop;

dying:
	/* Clean up */
	if (cas != NULL) {
		if (cas->cas_fnode != NULL)
			ct_free_fnode(cas->cas_fnode);
		if (cas->cas_handle != NULL)
			fclose(cas->cas_handle);
		e_free(&cas);
		op->op_priv = NULL;
	}
	return;
}

void
ct_xml_file_open(struct ct_global_state *state, struct ct_trans *trans,
    const char *file, int mode, uint32_t chunkno, ct_complete_fn callback)
{
	int	ret;

	trans->tr_state = TR_S_XML_OPEN;
	trans->tr_complete = callback;
	trans->tr_cleanup = NULL;

	if ((ret = ct_create_xml_open(&trans->hdr, (void **)&trans->tr_data[2],
	    file, mode, chunkno)) != 0) {
		ct_fatal(state, "can't create xml open packet: %s", ret);
		ct_trans_free(state, trans);
		return;
	}
	trans->tr_dataslot = 2;
	trans->tr_size[2] = trans->hdr.c_size;

	CNDBG(CT_LOG_XML, "open trans %"PRIu64, trans->tr_trans_id);
	ct_queue_first(state, trans);

}

int
ct_xml_file_open_polled(struct ct_global_state *state, const char *file,
    int mode, uint32_t chunkno)
{
#define ASSL_TIMEOUT 20
	struct ct_header	 hdr;
	void			*body = NULL;
	size_t			 sz;
	int			 rv;

	CNDBG(CT_LOG_XML, "setting up XML");

	if ((rv = ct_create_xml_open(&hdr, &body, file, mode, chunkno)) != 0)
		goto done;

	sz = hdr.c_size;
	/* use previous packet id so it'll fit with the state machine */
	hdr.c_tag = state->ct_packet_id - 1;
	ct_wire_header(&hdr);
	if (ct_assl_io_write_poll(state->ct_assl_ctx, &hdr, sizeof hdr,
	    ASSL_TIMEOUT) != sizeof hdr) {
		rv = CTE_SHORT_WRITE;
		goto done;
	}
	if (ct_assl_io_write_poll(state->ct_assl_ctx, body, sz,
	    ASSL_TIMEOUT) != sz) {
		rv = CTE_SHORT_WRITE;
		goto done;
	}
	e_free(&body);

	/* get server reply */
	if (ct_assl_io_read_poll(state->ct_assl_ctx, &hdr, sizeof hdr,
	    ASSL_TIMEOUT) != sizeof hdr) {
		rv = CTE_SHORT_READ;
		goto done;
	}
	ct_unwire_header(&hdr);

	/* we know the open was ok or bad, just read the body and dump it */
	body = e_calloc(1, hdr.c_size);
	if (ct_assl_io_read_poll(state->ct_assl_ctx, body, hdr.c_size,
	    ASSL_TIMEOUT) != hdr.c_size) {
		rv = CTE_SHORT_READ;
	} else if (hdr.c_status == C_HDR_S_OK &&
	    hdr.c_opcode == C_HDR_O_XML_REPLY) {
		rv = 0;
	}
	e_free(&body);

done:
	if (body)
		e_free(&body);
	return (rv);
#undef ASSL_TIMEOUT
}

int
ctfile_extract_complete_open(struct ct_global_state *state,
    struct ct_trans *trans)
{
	char	buffer[PATH_MAX]; /* XXX size? */
	int	ret;

	if ((ret = ctfile_xml_open_complete(state, trans)) != 0)
		goto done;

	if ((ret = ct_file_extract_open(state->extract_state,
	    trans->tr_fl_node)) != 0) {
		snprintf(buffer, sizeof(buffer), "unable to open file %s",
		    trans->tr_fl_node->fn_name);
		ct_fatal(state, buffer, ret);
	}

done:
	return (ret);
}

int
ctfile_extract_complete_read(struct ct_global_state *state, struct ct_trans
*trans)
{
	int	ret;

	/* ctfile reads only currently fail if the footer was wrong */
	if (trans->tr_errno != 0) {
		ct_fatal(state, "invalid ctfile read packet", trans->tr_errno);
		return (0);
	}
	CNDBG(CT_LOG_FILE, "writing packet sz %d",
	    trans->tr_size[(int)trans->tr_dataslot]);
	if ((ret = ct_file_extract_write(state->extract_state,
	    trans->tr_fl_node, trans->tr_data[(int)trans->tr_dataslot],
	    trans->tr_size[(int)trans->tr_dataslot])) != 0)
		/* fatal and return. we are done here */
		ct_fatal(state, "failed to write file", ret);
	return (0);
}

/*
 * Normal transaction cleanup function for ctfile_extract, we call
 * ct_free_fnode  to release our refcount on the fnode.
 */
void
ctfile_extract_cleanup_trans(struct ct_global_state *state,
    struct ct_trans *trans)
{
	/* release trans reference */
	ct_free_fnode(trans->tr_fl_node);
}

struct ctfile_extract_state {
	struct fnode	*ces_fnode;
	int		 ces_block_no;
	int		 ces_open_sent;
	int		 ces_is_open;
};

void
ctfile_extract(struct ct_global_state *state, struct ct_op *op)
{
	struct ct_ctfileop_args		*cca = op->op_args;
	struct ctfile_extract_state	*ces = op->op_priv;
	const char			*ctfile = cca->cca_localname;
	const char			*rname = cca->cca_remotename;
	struct ct_trans			*trans;
	struct ct_header		*hdr;
	int				 ret;

	if (state->ct_dying != 0)
		goto dying;

	switch (ct_get_file_state(state)) {
	case CT_S_STARTING:
		ces = e_calloc(1, sizeof(*ces));
		op->op_priv = ces;

		if (rname == NULL) {
			if ((rname = ctfile_cook_name(ctfile)) == NULL) {
				ct_fatal(state, ctfile,
				    CTE_INVALID_CTFILE_NAME);
				goto dying;
			}
			cca->cca_remotename = (char *)rname;
		}
		if ((ret = ct_file_extract_init(&state->extract_state,
		    cca->cca_tdir, 0, 0, 0, NULL, NULL)) != 0) {
			ct_fatal(state, "Can't initialize extract state",
			    ret);
			goto dying;
		}
		break;
	case CT_S_WAITING_SERVER:
		CNDBG(CT_LOG_FILE, "waiting on remote open");
		/* FALLTHROUGH */
	case CT_S_FINISHED:

		return;
	default:
		break;
	}
	ct_set_file_state(state, CT_S_RUNNING);

	trans = ct_trans_alloc(state);
	if (trans == NULL) {
		/* system busy, return */
		CNDBG(CT_LOG_TRANS, "ran out of transactions, waiting");
		ct_set_file_state(state, CT_S_WAITING_TRANS);
		return;
	}
	trans->tr_statemachine = ct_state_ctfile_extract;
	if (ces->ces_open_sent == 0) {
		ces->ces_fnode = ct_alloc_fnode();
		ces->ces_fnode->fn_type = C_TY_REG;
		ces->ces_fnode->fn_parent_dir =
		    ct_file_extract_get_rootdir(state->extract_state);
		ces->ces_fnode->fn_name = e_strdup(ctfile);
		ces->ces_fnode->fn_fullname = e_strdup(ctfile);
		ces->ces_fnode->fn_mode = S_IRUSR | S_IWUSR;
		ces->ces_fnode->fn_uid = getuid();
		ces->ces_fnode->fn_gid = getgid();
		ces->ces_fnode->fn_atime = time(NULL);
		ces->ces_fnode->fn_mtime = time(NULL);

		trans->tr_fl_node = ces->ces_fnode;

		ct_xml_file_open(state, trans, rname, MD_O_READ, 0,
		    ctfile_extract_complete_open);
		ces->ces_open_sent = 1;
		/* xml thread will wake us up when it gets the open */
		ct_set_file_state(state, CT_S_WAITING_SERVER);
		return;
	}

	ct_ref_fnode(ces->ces_fnode);
	trans->tr_fl_node = ces->ces_fnode;
	trans->tr_state = TR_S_EX_SHA;
	trans->tr_complete = ctfile_extract_complete_read;
	trans->tr_cleanup = ctfile_extract_cleanup_trans;
	trans->tr_type = TR_T_READ_CHUNK;
	trans->tr_eof = 0;
	trans->tr_ctfile_chunkno = ces->ces_block_no++;
	trans->tr_ctfile_name = rname;

	hdr = &trans->hdr;
	hdr->c_ex_status = 2;
	hdr->c_flags |= C_HDR_F_METADATA;

	if ((ret = ct_create_iv_ctfile(trans->tr_ctfile_chunkno, trans->tr_iv,
	    sizeof(trans->tr_iv))) != 0) {
		/* XXX add tr_ctfile_chunkno */
		ct_fatal(state, "ctfile iv", ret);
		goto dying;
	}
	ct_queue_first(state, trans);

	return;

dying:
	if (ces != NULL) {
		/*
		 * XXX can't free fnode, don't know if we're done with it
		 * or not...
		 */
		e_free(&ces);
		op->op_priv = NULL;
		if (state->extract_state != NULL) {
			ct_file_extract_cleanup(state->extract_state);
			state->extract_state = NULL;
		}
	}
	/* XXX can't free rname if we originally allocated it... */
	return;
}

int
ctfile_extract_complete_eof(struct ct_global_state *state,
    struct ct_trans *trans)
{
	ct_file_extract_close(state->extract_state,
	    trans->tr_fl_node);

	return (1); /* we are done here */
}

/*
 * EOF transaction cleanup function for ctfile_extract, we cleanup the extract
 * state since we are done with it then call ct_free_fnode  to release our
 * refcount on the fnode. WE call it twice since we now own have the file
 * threads refcount too.
 */
void
ctfile_extract_cleanup_eof(struct ct_global_state *state,
    struct ct_trans *trans)
{
	ct_file_extract_cleanup(state->extract_state);
	state->extract_state = NULL;
	ct_free_fnode(trans->tr_fl_node);
	ct_free_fnode(trans->tr_fl_node);
}

/*
 * deal with the oddities of the ctfile extract protocol. Called from the
 * read handler for ctfile when the server returns an error.
 */
void
ctfile_extract_handle_eof(struct ct_global_state *state, struct ct_trans *trans)
{
	int	ret;

	if (ct_get_file_state(state) != CT_S_FINISHED) {
		ct_set_file_state(state, CT_S_FINISHED);
		trans->tr_state = TR_S_XML_CLOSING;
		trans->tr_complete = ctfile_extract_complete_eof;
		trans->tr_cleanup = ctfile_extract_cleanup_eof;

		if ((ret = ct_create_xml_close(&trans->hdr,
		    (void **)&trans->tr_data[2])) != 0) {
			ct_fatal(state, "Could not create xml close packet",
			    ret);
			trans->tr_state = TR_S_XML_CLOSED;
			/* we still return here so that tr_cleanup will run */
			return;
		}
		trans->tr_dataslot = 2;
		trans->tr_size[2] = trans->hdr.c_size;
	} else {
		trans->tr_complete = ctfile_complete_noop;
		trans->tr_cleanup = NULL;
		/*
		 * We had > 1 ios in flight when we hit eof.
		 * We're already closing so just carry on and complete/free
		 * these when we're done.
		 * luckily since server requests complete in order these will
		 * all complete *before* the xml close above, despite having a
		 * higher sequence number. Therefore when we complete and
		 * free the transactions these trans will not be leaked.
		 */
		trans->tr_state = TR_S_XML_CLOSED;
	}
	/* queuing is handled by the caller. */
}

void
ctfile_list_start(struct ct_global_state *state, struct ct_op *op)
{
	struct ct_trans			*trans;
	int				 ret;

	if (ct_get_file_state(state) == CT_S_FINISHED ||
	    state->ct_dying != 0)
		return;

	trans = ct_trans_alloc(state);
	if (trans == NULL) {
		/* system busy, return */
		CNDBG(CT_LOG_TRANS, "ran out of transactions, waiting");
		ct_set_file_state(state, CT_S_WAITING_TRANS);
		return;
	}

	trans->tr_statemachine = ct_state_ctfile_list;
	trans->tr_state = TR_S_XML_LIST;

	if ((ret = ct_create_xml_list(&trans->hdr,
	    (void **)&trans->tr_data[2])) != 0) {
		ct_fatal(state, "Could not create xml list packet", ret);
		ct_trans_free(state, trans);
		return;
	}
	trans->tr_dataslot = 2;
	trans->tr_complete = ctfile_complete_noop_final;
	trans->tr_cleanup = NULL;
	trans->tr_size[2] = trans->hdr.c_size;

	ct_queue_first(state, trans);

	ct_set_file_state(state, CT_S_FINISHED);
}

/*
 * To be used in a completion handler for an operation.
 *
 * The operation which we are completing did a ctfile_list_start, the result of
 * this are in *files. perform any matching on this necessary using matchmode
 * with pattern flist and excludelist excludelist.  and place the results in
 * *results.
 *
 * If we return non-zero a fatal error occured..
 */
int
ctfile_list_complete(struct ctfile_list *files,  int matchmode, char **flist,
    char **excludelist, struct ctfile_list_tree *results)
{
	struct ct_match		*match, *ex_match = NULL;
	struct ctfile_list_file	*file;
	int			 ret, s_errno;

	if (SIMPLEQ_EMPTY(files))
		return (0);

	if ((ret = ct_match_compile(&match, matchmode, flist)) != 0) {
		s_errno = errno; /* save in case WARNX clobbers */
		CWARNX("couldn't compile match pattern: %s", ct_strerror(ret));
		errno = s_errno;
		return (ret);
	}
	if (excludelist && (ret = ct_match_compile(&ex_match, matchmode,
	    excludelist)) != 0) {
		s_errno = errno; /* save in case WARNX clobbers */
		CWARNX("couldn't compile exclude pattern: %s",
		    ct_strerror(ret));
		ct_match_unwind(match);
		errno = s_errno;

		/* XXX free list too? */
		return (ret);
	}

	while ((file = SIMPLEQ_FIRST(files)) != NULL) {
		SIMPLEQ_REMOVE_HEAD(files, mlf_link);
		if (ct_match(match, file->mlf_name) == 0 && (ex_match == NULL ||
		    ct_match(ex_match, file->mlf_name) == 1)) {
			RB_INSERT(ctfile_list_tree, results, file);
		} else {
			e_free(&file);
		}
	}
	if (ex_match != NULL)
		ct_match_unwind(ex_match);
	ct_match_unwind(match);

	return(0);
}

int
ctfile_delete_complete(struct ct_global_state *state,
    struct ct_trans *trans)
{
	struct ct_op			*op;
	struct ctfile_delete_args	*cda;

	op = ct_get_current_operation(state);
	if (op == NULL)
		CABORTX("no current operation while delete inprogress");
	cda = op->op_args;

	if (cda->cda_callback != NULL)
		cda->cda_callback(cda, state, trans);

	return (1);
}

void
ctfile_delete(struct ct_global_state *state, struct ct_op *op)
{
	struct ctfile_delete_args	*cda = op->op_args;
	const char			*rname;
	struct ct_trans			*trans;
	int				 ret;

	if (ct_get_file_state(state) == CT_S_FINISHED ||
	    state->ct_dying != 0)
		return;

	trans = ct_trans_alloc(state);
	if (trans == NULL) {
		/* system busy, return */
		CNDBG(CT_LOG_TRANS, "ran out of transactions, waiting");
		ct_set_file_state(state, CT_S_WAITING_TRANS);
		return;
	}
	trans->tr_statemachine = ct_state_ctfile_delete;
	trans->tr_state = TR_S_XML_DELETE;

	if ((rname = ctfile_cook_name(cda->cda_name)) == NULL) {
		ct_fatal(state, cda->cda_name, CTE_INVALID_CTFILE_NAME);
		ct_trans_free(state, trans);
		return;
	}

	if ((ret = ct_create_xml_delete(&trans->hdr,
	     (void **)&trans->tr_data[2], rname)) != 0) {
		ct_fatal(state, "Could not create xml delete packet", ret);
		ct_trans_free(state, trans);
		e_free(&rname);
		return;
	}
	trans->tr_dataslot = 2;
	trans->tr_complete = ctfile_delete_complete;
	trans->tr_cleanup = NULL;
	trans->tr_size[2] = trans->hdr.c_size;

	e_free(&rname);

	ct_queue_first(state, trans);

	ct_set_file_state(state, CT_S_FINISHED);
}

void
ct_handle_xml_reply(struct ct_global_state *state, struct ct_trans *trans,
    struct ct_header *hdr, void *vbody)
{
	int	 ret;
	int32_t	 newgenid;
	char	*filename;

	switch (trans->tr_state) {
	case TR_S_XML_OPEN:
		CNDBG(CT_LOG_NET, "got xml open reply");
		if ((ret = ct_parse_xml_open_reply(hdr, vbody,
		    &filename)) != 0) {
			ct_fatal(state, "failed to parse xml open reply", ret);
			goto just_queue;
		}
		if (filename == NULL) {
			ct_fatal(state, NULL, CTE_CANT_OPEN_REMOTE);
			goto just_queue;
		}
		CNDBG(CT_LOG_FILE, "%s opened\n",
		    filename);
		e_free(&filename);
		trans->tr_state = TR_S_XML_OPENED;
		break;
	case TR_S_XML_CLOSING:
		CNDBG(CT_LOG_NET, "got xml close reply");
		if ((ret = ct_parse_xml_close_reply(hdr, vbody)) != 0) {
			ct_fatal(state, "failed to parse xml close reply", ret);
			goto just_queue;
		}
		trans->tr_state = TR_S_DONE;
		break;
	case TR_S_XML_LIST:
		CNDBG(CT_LOG_NET, "got xml list reply");
		if ((ret = ct_parse_xml_list_reply(hdr, vbody,
		    &state->ctfile_list_files)) != 0) {
			ct_fatal(state, "failed to parse xml list reply", ret);
			goto just_queue;
		}
		trans->tr_state = TR_S_DONE;
		break;
	case TR_S_XML_DELETE:
		CNDBG(CT_LOG_NET, "got xml delete reply");
		if ((ret = ct_parse_xml_delete_reply(hdr, vbody,
		    &filename)) != 0) {
			ct_fatal(state, "failed to parse xml delete reply",
			    ret);
			goto just_queue;
		}
		trans->tr_ctfile_name = filename; /* whether NULL or not */
		trans->tr_state = TR_S_DONE;
		if (filename != NULL) {
		  e_free(&filename);
		}
		break;
	case TR_S_XML_CULL_SEND:
		/* XXX this is for both complete and setup */
		CNDBG(CT_LOG_NET, "got cull send reply");
		if ((ret = ct_parse_xml_cull_setup_reply(hdr, vbody)) != 0) {
			ct_fatal(state, "failed to parse cull setup reply",
			    ret);
			goto just_queue;
		}
		trans->tr_state = TR_S_DONE;
		break;
	case TR_S_XML_CULL_SHA_SEND:
		CNDBG(CT_LOG_NET, "got cull shas reply");
		if ((ret = ct_parse_xml_cull_shas_reply(hdr, vbody)) != 0) {
			ct_fatal(state, "failed to parse cull shas reply",
			    ret);
			goto just_queue;
		}
		if (trans->tr_eof == 1)
			trans->tr_state = TR_S_DONE;
		else
			trans->tr_state = TR_S_XML_CULL_REPLIED;
		break;
	case TR_S_XML_CULL_COMPLETE_SEND:
		CNDBG(CT_LOG_NET, "got cull complete reply");
		if ((ret = ct_parse_xml_cull_complete_reply(hdr, vbody,
		    &newgenid)) != 0) {
			ct_fatal(state, "failed to parse cull complete reply",
			    ret);
			goto just_queue;
		}
		ctdb_cull_end(state->ct_db_state, newgenid);
		trans->tr_state = TR_S_DONE;
		break;
	case TR_S_XML_EXT:
#if defined (CT_EXT_XML_REPLY_HANDLER)
		ret = CT_EXT_XML_REPLY_HANDLER(trans, hdr, vbody);
		if (ret) {
			ct_fatal(state, "failed to parse xml ext reply", ret);
		}
		trans->tr_state = TR_S_DONE;
		break;
#else
		/* FALLTHROUGH */
#endif
	default:
		CABORTX("unexpected transaction state %d", trans->tr_state);
	}

just_queue:
	ct_queue_transfer(state, trans);
	ct_body_free(state, vbody, hdr);
	ct_header_free(state, hdr);
}

/*
 * 1 - get list of ctfiles.
 * 2 - if we are checking them, grab *all* of them (silly, i know)
 * 3 - go through list sorting into ``to delete'' and ``not to delete''
 * 4 - go throug the not to delete file and check none of them have
 *     dependancies on files in the ``to delete'' list. fatal if so.
 * 4 - schedule deletions.
 * 5 - on completion of deletion, remove cachefile.
 */
ct_op_complete_cb	ctfile_process_delete;
ct_op_complete_cb	ctfile_delete_extract_cleanup;
ct_op_cb 		ctfile_delete_check_required;
ct_op_complete_cb	ctfile_delete_from_cache;

struct ct_delete_trees {
	struct ctfile_list_tree	all_files;
	struct ctfile_list_tree	delete_files;
};

int
ctfile_process_delete(struct ct_global_state *state, struct ct_op *op)
{
	struct ct_ctfile_delete_args	*ccda = op->op_args;
	struct ct_delete_trees		*trees;
	struct ct_match			*match;
	struct ctfile_list_file		*file, *tmp;
	struct ct_ctfileop_args		*cca;
	int				 ret;

	/*
	 * XXX In some way make sure we filter out crypto.secrets unless
	 * specifically mentioned.
	 */
	trees = e_calloc(1, sizeof(*trees));
	RB_INIT(&trees->all_files);
	RB_INIT(&trees->delete_files);
	if ((ret = ctfile_list_complete(&state->ctfile_list_files,
	    CT_MATCH_REGEX, all_ctfiles_pattern, NULL,
	    &trees->all_files)) != 0){
		goto dying;
	}

	if ((ret = ct_match_compile(&match, ccda->ccda_matchmode,
	    ccda->ccda_pattern)) != 0) {
		goto dying;
	}

	/* pass 1: separate out the files we intend to delete */
	RB_FOREACH_SAFE(file, ctfile_list_tree, &trees->all_files, tmp) {
		if (ct_match(match, file->mlf_name) == 0) {
			RB_REMOVE(ctfile_list_tree, &trees->all_files, file);
			RB_INSERT(ctfile_list_tree, &trees->delete_files, file);
		}
	}

	ct_match_unwind(match);

	if (RB_EMPTY(&trees->delete_files)) {
		ret = CTE_NOTHING_TO_DELETE;
		goto dying;
	}

	RB_FOREACH(file, ctfile_list_tree, &trees->all_files) {
		if (!ctfile_in_cache(file->mlf_name,
		    state->ct_config->ct_ctfile_cachedir)) {
			cca = e_calloc(1, sizeof(*cca));
			cca->cca_localname =  e_strdup(file->mlf_name);
			cca->cca_remotename = cca->cca_localname;
			cca->cca_tdir = state->ct_config->ct_ctfile_cachedir;
			cca->cca_ctfile = 1;
			ct_add_operation_after(state, op, ctfile_extract,
			    ctfile_delete_extract_cleanup, cca);
		}
	}

	op = ct_add_operation(state, ctfile_delete_check_required, NULL, ccda);
	op->op_priv = trees;

	return (0);

dying:
	while ((file = RB_ROOT(&trees->all_files)) != NULL) {
		RB_REMOVE(ctfile_list_tree, &trees->all_files, file);
		e_free(&file);
	}
	while ((file = RB_ROOT(&trees->delete_files)) != NULL) {
		RB_REMOVE(ctfile_list_tree, &trees->delete_files, file);
		e_free(&file);
	}
	return (ret);
}

int
ctfile_delete_extract_cleanup(struct ct_global_state *state, struct ct_op *op)
{
	struct ct_ctfileop_args *cca = op->op_args;

	e_free(&cca->cca_localname);
	e_free(&cca);

	return (0);
}

void
ctfile_delete_check_required(struct ct_global_state *state, struct ct_op *op)
{
	struct ct_ctfile_delete_args	*ccda = op->op_args;
	/* XXX pass this in as op->op_priv; */
	struct ct_delete_trees		*trees = op->op_priv;
	struct ctfile_list_file		*file, search, *prevfile;
	char				*prev_filename;
	int				 fail = 0, ret;

	if (ct_get_file_state(state) == CT_S_FINISHED)
		return;

	if (state->ct_dying != 0)
		goto dying;


	/*
	 * pass 2: go over the list of files we don't intend to delete and
	 * ensure that none of them depend on files in del_tree
	 */
	while ((file = RB_ROOT(&trees->all_files)) != NULL) {
		RB_REMOVE(ctfile_list_tree, &trees->all_files, file);

		if ((ret = ctfile_get_previous(file->mlf_name,
		    state->ct_config->ct_ctfile_cachedir,
		    &prev_filename)) != 0) {
			CWARNX("can not get previous file for %s: %s",
			    file->mlf_name, ct_strerror(ret));
			fail = 1;
			e_free(&file);
			continue;
		}
		if (prev_filename != NULL) {
			/* XXX basename? */
			strlcpy(search.mlf_name, prev_filename,
			    sizeof(search.mlf_name));
			if ((prevfile = RB_FIND(ctfile_list_tree,
			    &trees->delete_files, &search)) != NULL) {
				CWARNX("Can not delete %s, it is depended upon "
				    "by %s which is not scheduled for deletion",
				    prev_filename, file->mlf_name);
				/* continue until all are checked */
				fail = 1;
			}
			e_free(&prev_filename);
		}
		e_free(&file);
	}
	if (fail) {
		ct_fatal(state, NULL, CTE_CAN_NOT_DELETE);
		goto dying;
	}
	while ((file = RB_ROOT(&trees->delete_files)) != NULL) {
		struct ctfile_delete_args	*cda;

		RB_REMOVE(ctfile_list_tree, &trees->delete_files, file);

		cda = e_malloc(sizeof(*cda));
		cda->cda_name = e_strdup(file->mlf_name);
		cda->cda_callback = ccda->ccda_callback;

		ct_add_operation(state, ctfile_delete, ctfile_delete_from_cache,
		    cda);
		e_free(&file);

	}
	e_free(&trees);
	op->op_args = NULL;

	/* done with operation. next! */
	ct_set_file_state(state, CT_S_FINISHED);
	ct_op_complete(state);

	return;

dying:
	/* He's dead, Jim. Clean up*/
	while ((file = RB_ROOT(&trees->all_files)) != NULL) {
		RB_REMOVE(ctfile_list_tree, &trees->all_files, file);
		e_free(&file);
	}
	while ((file = RB_ROOT(&trees->delete_files)) != NULL) {
		RB_REMOVE(ctfile_list_tree, &trees->delete_files, file);
		e_free(&file);
	}

	e_free(&trees);
}

int
ctfile_delete_from_cache(struct ct_global_state *state, struct ct_op *op)
{
	struct ctfile_delete_args	*cda = op->op_args;

	/* remove the deleted file from cachedir. */
	(void)ctfile_cache_remove(cda->cda_name,
	    state->ct_config->ct_ctfile_cachedir);
	e_free(&cda->cda_name);
	e_free(&cda);

	return (0);
}

/*
 * Verify that the ctfile name is kosher for remote mode.
 * - Encode the name (with a fake prefix) to make sure it fits.
 * - To help with interoperability, scan for a few special characters
 *   and punt if we find those.
 */
int
ctfile_verify_name(char *ctfile)
{
	const char	*set = CT_CTFILE_REJECTCHRS;
	char		 b[CT_CTFILE_MAXLEN], b64[CT_CTFILE_MAXLEN];
	size_t		 span, ctfilelen;
	int		 sz;

	if (ctfile == NULL)
		return 1;

	sz = snprintf(b, sizeof(b), "YYYYMMDD-HHMMSS-%s", ctfile);
	if (sz == -1 || sz >= sizeof(b))
		return 1;

	/* Make sure it fits. */
	sz = ct_base64_encode(CT_B64_M_ENCODE, (uint8_t *)b, strlen(b),
	    (uint8_t *)b64, sizeof(b64));
	if (sz != 0)
		return 1;

	ctfilelen = strlen(ctfile);
	span = strcspn(ctfile, set);
	return !(span == ctfilelen);
}

/*
 * Data structures to hold cull data
 *
 * Should this be stored in memory or build a temporary DB
 * to hold it due to the number of shas involved?
 */

struct ct_sha_lookup ct_sha_rb_head = RB_INITIALIZER(&ct_sha_rb_head);
uint64_t shacnt;
uint64_t sha_payload_sz;

int ct_cmp_sha(struct sha_entry *, struct sha_entry *);

RB_PROTOTYPE(ct_sha_lookup, sha_entry, s_rb, ct_cmp_sha);
RB_GENERATE(ct_sha_lookup, sha_entry, s_rb, ct_cmp_sha);

int
ct_cmp_sha(struct sha_entry *d1, struct sha_entry *d2)
{
	return memcmp(d1->sha, d2->sha, sizeof (d1->sha));
}

int
ct_cull_handle_complete(struct ct_global_state *state, struct ct_trans *trans)
{
	if (trans->tr_eof == 0)
		ct_wakeup_file(state->event_state);

	return (trans->tr_eof != 0);
}

int
ct_cull_sha_insert(const uint8_t *sha)
{
	//char			shat[SHA_DIGEST_STRING_LENGTH];
	struct sha_entry	*node, *oldnode;
	int			 exists = 0;

	node = e_malloc(sizeof(*node));
	bcopy (sha, node->sha, sizeof(node->sha));

	//ct_sha1_encode((uint8_t *)sha, shat);
	//printf("adding sha %s\n", shat);

	oldnode = RB_INSERT(ct_sha_lookup, &ct_sha_rb_head, node);
	if (oldnode != NULL) {
		/* already present, throw away copy */
		e_free(&node);
		exists = 1;
	} else
		shacnt++;

	return (exists);
}

void
ct_cull_kick(struct ct_global_state *state)
{

	CNDBG(CT_LOG_TRANS, "add_op cull_setup");
	CNDBG(CT_LOG_SHA, "shacnt %" PRIu64 , shacnt);

	ct_add_operation(state, ctfile_list_start,
	    ct_cull_fetch_all_ctfiles, NULL);
	ct_add_operation(state, ct_cull_collect_ctfiles, NULL,  NULL);
	ct_add_operation(state, ct_cull_setup, NULL, NULL);
	ct_add_operation(state, ct_cull_send_shas, NULL, NULL);
	ct_add_operation(state, ct_cull_send_complete, ct_cull_complete, NULL);
}

int	ct_cull_add_shafile(struct ct_global_state *, const char *,
	    const char *);
int	ct_cull_sha_insert(const uint8_t *);

int
ct_cull_add_shafile(struct ct_global_state *state, const char *file,
    const char *cachedir)
{
	struct ctfile_parse_state	xs_ctx;
	char				*ct_next_filename;
	char				*ct_filename_free = NULL;
	char				*cachename;
	int				ret, s_errno = 0, ct_errno = 0, exists;

	CNDBG(CT_LOG_SHA, "processing [%s]", file);

	/*
	 * XXX - should we keep a list of added files,
	 * since we do files based on the list and 'referenced' files?
	 * rather than operating on files multiple times?
	 * might be useful for marking files at 'do not delete'
	 * (depended on by other MD archives.
	 */

next_file:
	ct_next_filename = NULL;

	/* filename may be absolute, or in cache dir */
	if (ct_absolute_path(file)) {
		cachename = e_strdup(file);
	} else {
		e_asprintf(&cachename, "%s%s", cachedir, file);
	}

	ret = ctfile_parse_init(&xs_ctx, cachename, cachedir);
	e_free(&cachename);
	CNDBG(CT_LOG_CTFILE, "opening [%s]", file);

	if (ret) {
		ct_fatal(state, file, ret);
		return (1);
	}

	if (ct_filename_free) {
		e_free(&ct_filename_free);
	}

	if (xs_ctx.xs_gh.cmg_prevlvl_filename) {
		CNDBG(CT_LOG_CTFILE, "previous backup file %s\n",
		    xs_ctx.xs_gh.cmg_prevlvl_filename);
		ct_next_filename = e_strdup(xs_ctx.xs_gh.cmg_prevlvl_filename);
		ct_filename_free = ct_next_filename;
	}

	do {
		ret = ctfile_parse(&xs_ctx);
		switch (ret) {
		case XS_RET_FILE:
			break;
		case XS_RET_FILE_END:
			/* nothing to do */
			break;
		case XS_RET_SHA:
			if (xs_ctx.xs_gh.cmg_flags & CT_MD_CRYPTO)
				exists = ct_cull_sha_insert(xs_ctx.xs_csha);
			else
				exists = ct_cull_sha_insert(xs_ctx.xs_sha);
			if (!exists)
				ctdb_cull_mark(state->ct_db_state,
				    xs_ctx.xs_sha);
			break;
		case XS_RET_EOF:
			break;
		case XS_RET_FAIL:
			s_errno = errno; /* save just in case */
			ct_errno = xs_ctx.xs_errno;
			break;
		}

	} while (ret != XS_RET_EOF && ret != XS_RET_FAIL);

	ctfile_parse_close(&xs_ctx);

	if (ret != XS_RET_EOF) {
		errno = s_errno; /* in case it is CTE_ERRNO */
		CWARNX("%s: %s", file, ct_strerror(ct_errno));
	} else {
		if (ct_next_filename) {
			file = ct_next_filename;
			goto next_file;
		}
	}
	return (0);
}

int
ct_cull_complete(struct ct_global_state *state, struct ct_op *op)
{
	CNDBG(CT_LOG_SHA, "shacnt %" PRIu64 " shapayload %" PRIu64, shacnt,
	    sha_payload_sz);

	return (0);
}

uint64_t cull_uuid; /* set up with random number in ct_cull_setup() */
/* tune this */
int sha_per_packet = 1000;

void
ct_cull_setup(struct ct_global_state *state, struct ct_op *op)
{
	struct ct_trans			*trans;
	int				 ret;

	if (ct_get_file_state(state) == CT_S_FINISHED)
		return;

	if (state->ct_dying != 0)
		goto dying;

	arc4random_buf(&cull_uuid, sizeof(cull_uuid));

	CNDBG(CT_LOG_SHA, "cull_setup");
	ct_set_file_state(state, CT_S_RUNNING);

	trans = ct_trans_alloc(state);
	if (trans == NULL) {
		CNDBG(CT_LOG_TRANS, "ran out of transactions, waiting");
		ct_set_file_state(state, CT_S_WAITING_TRANS);
		return;
	}
	trans->tr_statemachine = ct_state_cull;

	if ((ret = ct_create_xml_cull_setup(&trans->hdr,
	    (void **)&trans->tr_data[2], cull_uuid, CT_CULL_PRECIOUS)) != 0) {
		ct_fatal(state, "Could not create xml cull setup packet",
		    ret);
		goto dying;
	}
	trans->tr_dataslot = 2;
	trans->tr_size[2] = trans->hdr.c_size;
	trans->tr_state = TR_S_XML_CULL_SEND;
	trans->tr_complete = ct_cull_handle_complete;
	trans->tr_eof = 1;
	trans->tr_cleanup = NULL;

	ct_queue_first(state, trans);

	ct_set_file_state(state, CT_S_FINISHED);

	return;

dying:
	/* nothing to do here */
	return;
}

void
ct_cull_send_complete(struct ct_global_state *state, struct ct_op *op)
{
	struct ct_trans			*trans;
	int				 ret;

	if (ct_get_file_state(state) == CT_S_FINISHED)
		return;

	if (state->ct_dying != 0)
		goto dying;

	CNDBG(CT_LOG_SHA, "send cull_complete");
	trans = ct_trans_alloc(state);

	if (trans == NULL) {
		CNDBG(CT_LOG_TRANS, "ran out of transactions, waiting");
		ct_set_file_state(state, CT_S_WAITING_TRANS);
		return;
	}
	trans->tr_statemachine = ct_state_cull;

	if ((ret = ct_create_xml_cull_complete(&trans->hdr,
	    (void **)&trans->tr_data[2], cull_uuid, CT_CULL_PROCESS)) != 0) {
		ct_fatal(state, "Could not create xml cull setup packet",
		    ret);
		goto dying;
	}
	trans->tr_dataslot = 2;
	trans->tr_size[2] = trans->hdr.c_size;
	trans->tr_state = TR_S_XML_CULL_COMPLETE_SEND;
	trans->tr_eof = 1;
	trans->tr_complete = ct_cull_handle_complete;
	trans->tr_cleanup = NULL;

	ct_queue_first(state, trans);
	ct_set_file_state(state, CT_S_FINISHED);
	return;

dying:
	/* nothing to clean up here */
	return;
}


void
ct_cull_send_shas(struct ct_global_state *state, struct ct_op *op)
{
	struct ct_trans			*trans;
	int				 sha_add, ret, done = 0;

	if (ct_get_file_state(state) == CT_S_FINISHED)
		return;

	if (state->ct_dying != 0)
		goto dying;

	CNDBG(CT_LOG_SHA, "cull_send_shas");
	ct_set_file_state(state, CT_S_RUNNING);

	trans = ct_trans_alloc(state);

	if (trans == NULL) {
		CNDBG(CT_LOG_TRANS, "ran out of transactions, waiting");
		ct_set_file_state(state, CT_S_WAITING_TRANS);
		return;
	}

	trans->tr_statemachine = ct_state_cull;
	trans->tr_state = TR_S_XML_CULL_SHA_SEND;
	if ((ret = ct_create_xml_cull_shas(&trans->hdr,
	    (void **)&trans->tr_data[2], cull_uuid, &ct_sha_rb_head,
	    sha_per_packet, &sha_add)) != 0) {
		ct_fatal(state, "can't create cull shas packet", ret);
		goto dying;
	}
	shacnt -= sha_add;
	trans->tr_dataslot = 2;
	trans->tr_size[2] = trans->hdr.c_size;
	trans->tr_complete = ct_cull_handle_complete;
	trans->tr_cleanup = NULL;

	CNDBG(CT_LOG_SHA, "sending shas [%s]", (char *)trans->tr_data[2]);
	CNDBG(CT_LOG_SHA, "sending shas len %lu",
	    (unsigned long)trans->hdr.c_size);
	sha_payload_sz += trans->hdr.c_size;

	if (shacnt == 0 || RB_EMPTY(&ct_sha_rb_head)) {
		trans->tr_eof = 1;
		done = 1;
		CNDBG(CT_LOG_SHA, "shacnt %" PRIu64, shacnt);
	}
	ct_queue_first(state, trans);
	if (done) {
		ct_set_file_state(state, CT_S_FINISHED);
	}
	return;

dying:
	/* XXX free all remaining shas */
	return;
}

/*
 * Code to get all metadata files on the server.
 * to be used for cull.
 */
struct ctfile_list_tree ct_cull_all_ctfiles =
    RB_INITIALIZER(&ct_cull_all_ctfiles);

ct_op_complete_cb	ct_cull_extract_cleanup;
int
ct_cull_fetch_all_ctfiles(struct ct_global_state *state, struct ct_op *op)
{
	struct ct_ctfileop_args	*cca;
	struct ctfile_list_tree	 results;
	struct ctfile_list_file	*file;
	char			*cachename;
	int			 ret;

	RB_INIT(&results);
	if ((ret = ctfile_list_complete(&state->ctfile_list_files,
	    CT_MATCH_REGEX, all_ctfiles_pattern, NULL, &results)) != 0)
		return (ret);

	while ((file = RB_ROOT(&results)) != NULL) {
		RB_REMOVE(ctfile_list_tree, &results, file);
		CNDBG(CT_LOG_CTFILE, "looking for file %s ", file->mlf_name);
		if (!ctfile_in_cache(file->mlf_name,
		    state->ct_config->ct_ctfile_cachedir)) {
			cachename = ctfile_get_cachename(file->mlf_name,
			    state->ct_config->ct_ctfile_cachedir);
			CNDBG(CT_LOG_CTFILE, "getting %s to %s", file->mlf_name,
			    cachename);
			e_free(&cachename);
			cca = e_calloc(1, sizeof(*cca));
			cca->cca_localname =  e_strdup(file->mlf_name);
			cca->cca_remotename = cca->cca_localname;
			cca->cca_tdir = state->ct_config->ct_ctfile_cachedir;
			cca->cca_ctfile = 1;
			ct_add_operation_after(state, op, ctfile_extract,
			    ct_cull_extract_cleanup, cca);
		} else {
			CNDBG(CT_LOG_CTFILE, "already got %s", file->mlf_name);
		}
		RB_INSERT(ctfile_list_tree, &ct_cull_all_ctfiles, file);
	}
	return (0);
}

int
ct_cull_extract_cleanup(struct ct_global_state *state, struct ct_op *op)
{
	struct ct_ctfileop_args *cca = op->op_args;

	e_free(&cca->cca_localname);
	e_free(&cca);

	return (0);
}

void
ct_cull_collect_ctfiles(struct ct_global_state *state, struct ct_op *op)
{
	struct ctfile_list_file	*file, *prevfile, filesearch;
	char			*prev_filename;
	int			timelen;
	char			buf[TIMEDATA_LEN];
	time_t			now;
	int			keep_files = 0, total_files = 0, ret = 0;

	if (ct_get_file_state(state) == CT_S_FINISHED)
		return;

	if (state->ct_dying != 0)
		goto dying;

	if (state->ct_config->ct_ctfile_keep_days == 0) {
		ct_fatal(state, "cull: ctfile_cull_keep_days",
		    CTE_MISSING_CONFIG_VALUE);
		goto dying;
	}
	CNDBG(CT_LOG_SHA, "collecting ctfiles");

	now = time(NULL);
	now -= (24 * 60 * 60 * state->ct_config->ct_ctfile_keep_days);
	if (strftime(buf, TIMEDATA_LEN, "%Y%m%d-%H%M%S",
	    localtime(&now)) == 0)
		CABORTX("can't format time");

	timelen = strlen(buf);

	RB_FOREACH(file, ctfile_list_tree, &ct_cull_all_ctfiles) {
		total_files++;
		if (strncmp (file->mlf_name, buf, timelen) < 0) {
			file->mlf_keep = 0;
		} else {
			file->mlf_keep = 1;
			keep_files++;
		}
	}

	/*
	 * It is ok to have no ctfiles at all and want to nuke all data.
	 * but we assume that if you are culling all your data then you
	 * probably didn't want to do that.
	 */
	if (keep_files == 0 && total_files != 0) {
		ct_fatal(state, NULL, CTE_CULL_EVERYTHING);
		goto dying;
	}

	RB_FOREACH(file, ctfile_list_tree, &ct_cull_all_ctfiles) {
		if (file->mlf_keep == 0)
			continue;

		if ((ret = ctfile_get_previous(file->mlf_name,
		    state->ct_config->ct_ctfile_cachedir,
		    &prev_filename)) != 0) {
			CWARNX("can not get previous file for %s",
			    file->mlf_name);
			ct_fatal(state, NULL, ret);
			goto dying;
		}
prev_ct_file:
		if (prev_filename != NULL) {
			CINFO("prev filename %s", prev_filename);
			strncpy(filesearch.mlf_name, prev_filename,
			    sizeof(filesearch.mlf_name));
			prevfile = RB_FIND(ctfile_list_tree,
			    &ct_cull_all_ctfiles, &filesearch);
			if (prevfile == NULL) {
				CWARNX("file not found in ctfilelist [%s]",
				    prev_filename);
			} else {
				if (prevfile->mlf_keep == 0)
					CINFO("Warning, old ctfile %s still "
					    "referenced by newer backups, "
					    "keeping", prev_filename);
				prevfile->mlf_keep++;
				e_free(&prev_filename);

				if ((ret =
				    ctfile_get_previous(prevfile->mlf_name,
				    state->ct_config->ct_ctfile_cachedir,
				    &prev_filename)) != 0) {
					/* XXX Fail? */
					CWARNX("can not get previous file for"
					    "%s", prevfile->mlf_name);
					goto dying;
				}
				goto prev_ct_file;
			}
			e_free(&prev_filename);
		}
	}

	ctdb_cull_start(state->ct_db_state);
	RB_FOREACH(file, ctfile_list_tree, &ct_cull_all_ctfiles) {
		if (file->mlf_keep == 0) {
			struct ctfile_delete_args	*cda;
			CNDBG(CT_LOG_CTFILE, "adding %s to delete list",
			    file->mlf_name);
			cda = e_calloc(1, sizeof(*cda));
			cda->cda_name = e_strdup(file->mlf_name);
			ct_add_operation(state, ctfile_delete,
			    ctfile_delete_from_cache, cda);
		} else {
			CNDBG(CT_LOG_CTFILE, "adding %s to keep list",
			    file->mlf_name);
			if (ct_cull_add_shafile(state, file->mlf_name,
			    state->ct_config->ct_ctfile_cachedir) != 0)
				goto dying;
		}
	}

	/* cleanup */
	while((file = RB_ROOT(&ct_cull_all_ctfiles)) != NULL) {
		RB_REMOVE(ctfile_list_tree, &ct_cull_all_ctfiles, file);
		e_free(&file);
		/* XXX - name  */
	}
	CNDBG(CT_LOG_SHA, "collected ctfiles");
	ct_op_complete(state);

	return;

dying:
	/* XXX cleanup the cull tree */
	return;
}
