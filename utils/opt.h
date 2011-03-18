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

typedef struct opt_struct *Opt;

Opt             opt_create (void);
void            opt_destroy (Opt o);
char           *opt_string (Opt o);
int             opt_add (Opt o, const char *fmt, ...);
void            opt_add_override (Opt o, const char *fmt, ...);
int             opt_add_cslist (Opt o, const char *s);
void            opt_add_cslist_override (Opt o, const char *s);
char           *opt_find (Opt o, char *key);
int             opt_delete (Opt o, char *key);
int             opt_scan (Opt o, const char *fmt, ...);
int             opt_check_allowed_cslist (Opt o, const char *s);


/*
 * vi:tabstop=4 shiftwidth=4 expandtab
 */