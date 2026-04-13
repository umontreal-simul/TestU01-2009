%.h: ${MYLIB}/%.tex
	${top_builddir}/tcode $< $@

%.h: ${PROBDIST}/%.tex
	${top_builddir}/tcode $< $@

%.h: ${TESTU01}/%.tex
	${top_builddir}/tcode $< $@

include Makefile.def

include_HEADERS = $(MYLIBHEADERS) $(PROBDISTHEADERS) $(TESTU01HEADERS) gdefconf.h

noinst_HEADERS = $(MYLIBTEX) $(PROBDISTTEX) $(TESTU01TEX)
noinst_SCRIPTS = makedef

CLEANFILES = $(MYLIBHEADERS) $(PROBDISTHEADERS) $(TESTU01HEADERS) gdefconf.h
MAINTAINERCLEANFILES = Makefile.in Makefile.def
