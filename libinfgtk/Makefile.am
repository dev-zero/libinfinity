# TODO: Find a way to have the version number set automatically.
lib_LTLIBRARIES = libinfgtk-0.7.la

cppflags = \
	-I$(top_srcdir) \
	$(infgtk_CFLAGS) \
	$(infinity_CFLAGS)

ldflags = \
	-no-undefined \
	-version-info $(LIBINFINITY_LIBTOOL_VERSION)

libadd = \
	${top_builddir}/libinfinity/libinfinity-$(LIBINFINITY_API_VERSION).la \
	$(infgtk_LIBS) \
	$(infinity_LIBS)

headers = \
	inf-gtk-account-creation-dialog.h \
	inf-gtk-acl-sheet-view.h \
	inf-gtk-browser-model.h \
	inf-gtk-browser-model-filter.h \
	inf-gtk-browser-model-sort.h \
	inf-gtk-browser-store.h \
	inf-gtk-browser-view.h \
	inf-gtk-certificate-dialog.h \
	inf-gtk-certificate-manager.h \
	inf-gtk-certificate-view.h \
	inf-gtk-chat.h \
	inf-gtk-io.h \
	inf-gtk-permissions-dialog.h

sources = \
	inf-gtk-account-creation-dialog.c \
	inf-gtk-acl-sheet-view.c \
	inf-gtk-browser-model.c \
	inf-gtk-browser-model-filter.c \
	inf-gtk-browser-model-sort.c \
	inf-gtk-browser-store.c \
	inf-gtk-browser-view.c \
	inf-gtk-certificate-dialog.c \
	inf-gtk-certificate-manager.c \
	inf-gtk-certificate-view.c \
	inf-gtk-chat.c \
	inf-gtk-io.c \
	inf-gtk-permissions-dialog.c

libinfgtk_0_7_la_CPPFLAGS = $(cppflags)
libinfgtk_0_7_la_LDFLAGS = $(ldflags)
libinfgtk_0_7_la_LIBADD = $(libadd)
libinfgtk_0_7_la_HEADERS = $(headers)
libinfgtk_0_7_la_SOURCES = $(sources)
libinfgtk_0_7_ladir = \
	$(includedir)/libinfgtk-$(LIBINFINITY_API_VERSION)/libinfgtk
