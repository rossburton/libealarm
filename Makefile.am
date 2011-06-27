lib_LTLIBRARIES = libealarm.la

libealarm_la_CPPFLAGS =			\
	-DG_LOG_DOMAIN=\"libealarm\"	\
	$(AM_CPPFLAGS)			\
	$(WARN_CFLAGS)			\
	$(ECAL_CFLAGS)

libealarm_la_SOURCES =			\
	alarm.c alarm.h \
	alarm-queue.c alarm-queue.h \
	alarm-notify.c alarm-notify.h \
	config-data.c config-data.h

libealarm_la_LIBADD =			\
	$(ECAL_LIBS)

libealarm_la_LDFLAGS = $(NO_UNDEFINED)

libealarmincludedir = $(includedir)/ealarm
libealarminclude_HEADERS = alarm-notify.h

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = libealarm.pc
EXTRA_DIST = $(pkgconfig_DATA:.pc=.pc.in)

noinst_PROGRAMS = test-alarm
test_alarm_SOURCES = test-alarm.c
test_alarm_CFLAGS = \
	$(AM_CPPFLAGS) \
	$(ECAL_CFLAGS)
test_alarm_LDADD = \
	libealarm.la \
	$(ECAL_LIBS)


MAINTAINERCLEANFILES =			\
	$(srcdir)/INSTALL		\
	$(srcdir)/aclocal.m4		\
	$(srcdir)/compile		\
	$(srcdir)/config.guess		\
	$(srcdir)/config.h.in		\
	$(srcdir)/config.sub		\
	$(srcdir)/depcomp		\
	$(srcdir)/install-sh		\
	$(srcdir)/ltmain.sh		\
	$(srcdir)/missing

-include $(top_srcdir)/git.mk
