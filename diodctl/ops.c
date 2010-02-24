/*****************************************************************************
 *  Copyright (C) 2010 Lawrence Livermore National Security, LLC.
 *  Written by Jim Garlick <garlick@llnl.gov> LLNL-CODE-423279
 *  All Rights Reserved.
 *
 *  This file is part of the Distributed I/O Daemon (diod).
 *  For details, see <http://code.google.com/p/diod/>.
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License (as published by the
 *  Free Software Foundation) version 2, dated June 1991.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the IMPLIED WARRANTY OF MERCHANTABILITY
 *  or FITNESS FOR A PARTICULAR PURPOSE. See the terms and conditions of the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software Foundation,
 *  Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA or see
 *  <http://www.gnu.org/licenses/>.
 *****************************************************************************/

/* ops.c - file ops for diodctl */

#if HAVE_CONFIG_H
#include "config.h"
#endif
#define _BSD_SOURCE         /* daemon */
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#if HAVE_GETOPT_H
#include <getopt.h>
#endif
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/param.h>
#include <string.h>
#include <sys/resource.h>
#include <poll.h>
#include <assert.h>

#include "npfs.h"
#include "list.h"

#include "diod_log.h"
#include "diod_conf.h"
#include "diod_trans.h"
#include "diod_upool.h"
#include "diod_sock.h"

#include "serv.h"

static Npfile       *_ctl_root_create (void);
static Npfcall      *_ctl_attach (Npfid *fid, Npfid *nafid, Npstr *uname,
                                  Npstr *aname);
static Npfcall      *_ctl_version (Npconn *conn, u32 msize, Npstr *version);

void
diodctl_register_ops (Npsrv *srv)
{
    npfile_init_srv (srv, _ctl_root_create ());
    srv->debuglevel = diod_conf_get_debuglevel ();
    srv->debugprintf = msg;
    srv->upool = diod_upool;
    srv->version = _ctl_version;
    srv->attach = _ctl_attach;
}

/* Tversion - negotiate 9P protocol version (9P2000.u or bust).
 */
static Npfcall*
_ctl_version (Npconn *conn, u32 msize, Npstr *version)
{
    Npfcall *ret = NULL;

    if (np_strcmp (version, "9P2000.h") && np_strcmp (version, "9P2000.u")) {
        np_werror ("unsupported 9P version", EIO);
        goto done;
    }
    if (msize < IOHDRSZ + 1) {
        np_werror ("msize too small", EIO);
        goto done;
    }
    if (msize > conn->srv->msize)
        msize = conn->srv->msize;

    np_conn_reset (conn, msize, 1); /* 1 activates 'dotu' */
    if (!(ret = np_create_rversion (msize, "9P2000.u"))) {
        np_uerror (ENOMEM);
        goto done;
    }
done:
    return ret;
}

/* Tattach - announce a new user, and associate her fid with the root dir.
 */
static Npfcall*
_ctl_attach (Npfid *fid, Npfid *nafid, Npstr *uname, Npstr *aname)
{
    char *host = diod_trans_get_host (fid->conn->trans);
    char *ip = diod_trans_get_ip (fid->conn->trans);
    Npfile *root = (Npfile *)fid->conn->srv->treeaux;
    Npfcall *ret = NULL;
    Npfilefid *f;
    uid_t auid;

    if (nafid) {    /* 9P Tauth not supported */
        np_werror (Enoauth, EIO);
        goto done;
    }
    if (np_strcmp (aname, "/diodctl") != 0) {
        np_uerror (EPERM);
        goto done;
    }
    /* Munge authentication involves the upool and trans layers:
     * - we ask the upool layer if the user now attaching has a munge cred
     * - we stash the uid of the last successful munge auth in the trans layer
     * - subsequent attaches on the same trans get to leverage the last auth
     * By the time we get here, invalid munge creds have already been rejected.
     */
    if (diod_conf_get_munge ()) {
        if (diod_user_has_mungecred (fid->user)) {
            diod_trans_set_authuser (fid->conn->trans, fid->user->uid);
        } else {
            if (diod_trans_get_authuser (fid->conn->trans, &auid) < 0) {
                np_uerror (EPERM);
                goto done;
            }
            if (auid != 0 && auid != fid->user->uid) {
                np_uerror (EPERM);
                goto done;
            }
        }
    }
    if (!npfile_checkperm (root, fid->user, 4)) {
        np_uerror (EPERM);
        goto done;
    }
    if (!(f = npfile_fidalloc (root, fid))) {
        np_uerror (ENOMEM);
        goto done;
    }
    if (!(ret = np_create_rattach (&root->qid))) {
        np_uerror (ENOMEM);
        goto done;
    }
    fid->aux = f;
    np_fid_incref (fid);
done:
    msg ("attach user %s path /diodctl host %s(%s): %s",
         fid->user->uname, host, ip, np_haserror () ? "DENIED" : "ALLOWED");
    if (np_haserror ())
        npfile_fiddestroy (fid); /* frees fid->aux as Npfilefid* if not NULL */
    return ret;
}

/* Callback for root dir.
 */
static Npfile *
_root_first (Npfile *dir)
{
    if (dir->dirfirst)
        npfile_incref(dir->dirfirst);

    return dir->dirfirst;
}

/* Callback for root dir.
 */
static Npfile *
_root_next (Npfile *dir, Npfile *prevchild)
{
    if (prevchild->next)
        npfile_incref (prevchild->next);

    return prevchild->next;
}

/* Handle a read from the 'exports' file.
 */
static int
_exports_read (Npfilefid *f, u64 offset, u32 count, u8 *data, Npreq *req)
{
    char *buf = f->file->aux;
    int cpylen = strlen (buf) - offset;

    if (cpylen > count)
        cpylen = count;
    if (cpylen < 0)
        cpylen = 0;
        memcpy (data, buf + offset, cpylen);
    return cpylen;
}

/* Handle a read from the 'server' file.
 */
static int
_server_read (Npfilefid* file, u64 offset, u32 count, u8* data, Npreq *req)
{
    Npfid *fid = file->fid;

    return diodctl_serv_getname (fid->user, offset, count, data);
}

/* Handle a write to the 'ctl' file.
 * Content of the write is ignored since we only have one action.
 */
static int
_ctl_write (Npfilefid* file, u64 offset, u32 count, u8* data, Npreq *req)
{
    Npfid *fid = file->fid;
    char *ip = diod_trans_get_ip (fid->conn->trans);

    if (diodctl_serv_create (fid->user, ip))
        return count;
    return 0;
}

/* A no-op (no error) wstat.
 */
static int
_noop_wstat (Npfile* file, Npstat* stat)
{
    return 1; /* 0 = fail */
}

static Npdirops root_ops = {
        .first = _root_first,
        .next =  _root_next,
};
static Npfileops exports_ops = {
        .read  = _exports_read,
};
static Npfileops server_ops = {
        .read  = _server_read,
};
static Npfileops ctl_ops = {
        .write = _ctl_write,
        .wstat = _noop_wstat, /* needed because mtime is set before a write */
};

/* Create the file system representation for /diodctl.
 */
static Npfile *
_ctl_root_create (void)
{
    Npfile *root, *exports, *server, *ctl;
    Npuser *user;
    char *tmpstr;

    if (!(user = diod_upool->uid2user (diod_upool, 0)))
        msg_exit ("out of memory");

    if (!(tmpstr = strdup ("")))
        msg_exit ("out of memory");
    if (!(root = npfile_alloc (NULL, tmpstr, 0555|Dmdir, 0, &root_ops, NULL)))
        msg_exit ("out of memory");
    root->parent = root;
    npfile_incref(root);
    root->atime = time(NULL);
    root->mtime = root->atime;
    root->uid = user;
    root->gid = user->dfltgroup;
    root->muid = user;

    if (!(tmpstr = strdup ("exports")))
        msg_exit ("out of memory");
    if (!(exports = npfile_alloc(root, tmpstr, 0444, 1, &exports_ops, NULL)))
        msg_exit ("out of memory");
    npfile_incref(exports);
    if (!(exports->aux = diod_conf_cat_exports ()))
        msg_exit ("out of memory");

    if (!(tmpstr = strdup ("server")))
        msg_exit ("out of memory");
    if (!(server = npfile_alloc(root, tmpstr, 0444, 1, &server_ops, NULL)))
        msg_exit ("out of memory");
    npfile_incref(server);

    if (!(tmpstr = strdup ("ctl")))
        msg_exit ("out of memory");
    if (!(ctl = npfile_alloc(root, tmpstr, 0666, 1, &ctl_ops, NULL)))
        msg_exit ("out of memory");
    npfile_incref(ctl);

    root->dirfirst = exports;
    exports->next = server;
    server->next = ctl;
    root->dirlast = ctl;

    return root;
}

/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */
