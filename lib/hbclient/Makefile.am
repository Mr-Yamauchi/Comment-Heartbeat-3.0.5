#
# hbclient library: Linux-HA heartbeat code
#
# Copyright (C) 2001 Michael Moerz
# Copyright (C) 2004 International Business Machines
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
# 
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# 
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
#
MAINTAINERCLEANFILES    = Makefile.in

INCLUDES                = -I$(top_builddir)/include -I$(top_srcdir)/include \
			-I$(top_builddir)/linux-ha -I$(top_srcdir)/linux-ha \
			-I$(top_builddir)/libltdl -I$(top_srcdir)/libltdl

hadir			= $(sysconfdir)/ha.d
halibdir		= $(libdir)/@HB_PKG@
commmoddir		= $(halibdir)/modules/comm
havarlibdir		= $(localstatedir)/lib/@HB_PKG@
havarrundir		= $(localstatedir)/run/
apidir		       	= $(havarlibdir)/api
apicasualdir		= $(havarlibdir)/casual
# fifos with path
hafifo			= $(havarlibdir)/fifo
apigid			= @HA_APIGID@
gliblib			= @GLIBLIB@

LIBRT 			= @LIBRT@
AM_CFLAGS		= @CFLAGS@

## libraries

lib_LTLIBRARIES		= libhbclient.la 

libhbclient_la_SOURCES	= client_lib.c
libhbclient_la_LDFLAGS	= -version-info 1:0:0
libhbclient_la_LIBADD	= $(top_builddir)/replace/libreplace.la  

## binary progs
halib_PROGRAMS		= api_test

api_test_SOURCES	= api_test.c
api_test_LDADD		= -lplumb	\
			libhbclient.la $(gliblib) \
			-lpils


