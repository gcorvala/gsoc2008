AM_CPPFLAGS =						\
	-I$(top_srcdir)                          	\
	-DPREFIX="\"$(prefix)"\"			\
	-DSYSCONFDIR=\""$(sysconfdir)"\"		\
	-DDATADIR=\""$(datadir)"\"              	\
	-DPKGDATADIR=\""$(pkgdatadir)"\"		\
	-DLIBDIR=\""$(libdir)"\"			\
	-DLOCALEDIR=\""$(datadir)/locale"\"		\
	$(EMPATHY_CFLAGS)				\
	$(LIBEMPATHY_CFLAGS) \
	$(WARN_CFLAGS)

LDADD =								\
	$(top_builddir)/libempathy-gtk/libempathy-gtk.la	\
	$(top_builddir)/libempathy/libempathy.la		\
	$(top_builddir)/extensions/libemp-extensions.la		\
	$(EMPATHY_LIBS) \
	$(LIBEMPATHY_LIBS)

bin_PROGRAMS =			\
	empathy			\
	empathy-accounts	\
	empathy-logs

test_SOURCES = \
	empathy.c							\
	bacon-message-connection.c bacon-message-connection.h		\
	empathy-chat-window.c empathy-chat-window.h			\
	empathy-new-chatroom-dialog.c empathy-new-chatroom-dialog.h	\
	empathy-status-icon.c empathy-status-icon.h			\
	empathy-about-dialog.c empathy-about-dialog.h			\
	empathy-chatrooms-window.c empathy-chatrooms-window.h		\
	empathy-main-window.c empathy-main-window.h			\
	empathy-preferences.c empathy-preferences.h			\
	empathy-call-window.c empathy-call-window.h			\
	ephy-spinner.c ephy-spinner.h \
	empathy-tp-call.c empathy-tp-call.h \
	empathy-tp-audio-stream.c empathy-tp-audio-stream.h

empathy_SOURCES =							\
	$(test_SOURCES) \
	empathy-marshal.c empathy-marshal.h

empathy_accounts_SOURCES = empathy-accounts.c
empathy_logs_SOURCES = empathy-logs.c

gladedir = $(datadir)/empathy
glade_DATA = 					\
	empathy-call-window.glade		\
	empathy-main-window.glade		\
	empathy-preferences.glade		\
	empathy-chatrooms-window.glade		\
	empathy-chat-window.glade		\
	empathy-new-chatroom-dialog.glade	\
	empathy-status-icon.glade

dist_man_MANS =			\
	empathy.1		\
	empathy-accounts.1

EXTRA_DIST =			\
	empathy-marshal.list \
	$(autostart_DATA)	\
	$(glade_DATA)

CLEANFILES = $(BUILT_SOURCES)

BUILT_SOURCES = \
	empathy-marshal.h	\
	empathy-marshal.c \
	empathy-marshal.list

empathy-marshal.list: $(test_SOURCES) Makefile.am
	( cd $(srcdir) && \
	sed -n -e 's/.*empathy_marshal_\([[:upper:][:digit:]]*__[[:upper:][:digit:]_]*\).*/\1/p' \
	$(test_SOURCES) ) \
	| sed -e 's/__/:/' -e 'y/_/,/' | sort -u > $@.tmp
	if cmp -s $@.tmp $@; then \
		rm $@.tmp; \
	else \
		mv $@.tmp $@; \
	fi

%-marshal.h: %-marshal.list Makefile
	$(GLIB_GENMARSHAL) --header --prefix=_$(subst -,_,$*)_marshal $< > $*-marshal.h

%-marshal.c: %-marshal.list Makefile
	echo "#include \"empathy-marshal.h\"" > $@ && \
	$(GLIB_GENMARSHAL) --body --prefix=_$(subst -,_,$*)_marshal $< >> $*-marshal.c
