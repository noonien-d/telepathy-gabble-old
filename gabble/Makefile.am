if ENABLE_PLUGIN_API

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = telepathy-gabble.pc

gabbleincludedir = $(includedir)/telepathy-gabble-0/gabble
gabbleinclude_HEADERS = $(headers)

else

EXTRA_DIST = $(headers)

endif

headers = \
    capabilities.h \
    capabilities-set.h \
    caps-channel-manager.h \
    caps-hash.h \
    plugin-connection.h \
    error.h \
    gabble.h \
    namespaces.h \
    plugin.h \
    sidecar.h \
    types.h
