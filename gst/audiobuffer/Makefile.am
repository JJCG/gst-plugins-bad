plugin_LTLIBRARIES = libgstaudiobuffer.la

libgstaudiobuffer_la_SOURCES = gstaudioringbuffer.c
libgstaudiobuffer_la_CFLAGS = $(GST_PLUGINS_BASE_CFLAGS) $(GST_BASE_CFLAGS) \
    $(GST_CFLAGS) 
libgstaudiobuffer_la_LIBADD = $(GST_PLUGINS_BASE_LIBS) $(GST_BASE_LIBS) \
			   -lgstaudio-$(GST_MAJORMINOR)

libgstaudiobuffer_la_LDFLAGS = $(GST_PLUGIN_LDFLAGS)
libgstaudiobuffer_la_LIBTOOLFLAGS = --tag=disable-static

