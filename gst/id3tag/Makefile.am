plugin_LTLIBRARIES = libgstid3tag.la

libgstid3tag_la_SOURCES = \
	gstid3mux.c \
	gsttagmux.c \
	id3tag.c

libgstid3tag_la_CFLAGS = \
	$(GST_PLUGINS_BASE_CFLAGS) \
	$(GST_CFLAGS)

libgstid3tag_la_LIBADD = \
	$(GST_PLUGINS_BASE_LIBS) -lgsttag-$(GST_MAJORMINOR) \
	$(GST_LIBS)

libgstid3tag_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)
libgstid3tag_la_LIBTOOLFLAGS = --tag=disable-static

noinst_HEADERS = gstid3mux.h gsttagmux.h id3tag.h
