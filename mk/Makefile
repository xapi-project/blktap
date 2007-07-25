USE_BRANDING=yes
include $(B_BASE)/common.mk
include $(B_BASE)/rpmbuild.mk

REPO_NAME= blktap
REPO=$(call hg_loc,blktap)
XEN_ROOT=$(call pq_loc,xs-xen)
RPM_BUILD_COOKIE= $(MY_OBJ_DIR)/.rpm_build_cookie

J_FLAG = 8
MAKE_CMD= $(MAKE) -j$(J_FLAG) -C $(REPO)

-include $(MY_OBJ_DIR)/version.inc
$(MY_OBJ_DIR)/version.inc:
	rm -f $(MY_OBJ_DIR)/version.inc
	$(version-makefile) > $@
	$(call hg_cset_number,blktap) >> $@
	echo BLKTAP_VERSION := \$$\(PRODUCT_VERSION\) >> $@
	echo BLKTAP_RELEASE := \$$\(CSET_NUMBER\) >> $@

CHANGELOG_DATE    := $(shell LANG=C date +"%a %b %d %Y")
CHANGELOG_USER    := XenSource, Ltd. <xensource.com>
CHANGELOG_HEADER  := * $(CHANGELOG_DATE) $(CHANGELOG_USER) [$(BLKTAP_VERSION) $(BLKTAP_RELEASE)]
CHANGELOG_MESSAGE := - Build blktap.

SOURCES = $(RPM_SOURCESDIR)/blktap-$(BLKTAP_VERSION).tar.bz2 \
          $(RPM_SOURCESDIR)/blktap-development.patch \
          $(RPM_SPECSDIR)/blktap.spec

.PHONY: build
build: $(RPM_BUILD_COOKIE) $(MY_SOURCES)/MANIFEST
	@ :

.PHONY: clean
clean: 
	rm -f $(RPM_BUILD_COOKIE)
	rm -f $(MY_OBJ_DIR)/version.inc
	rm -f $(SOURCES) $(SOURCES:%=%.tainted)
	rm -f $(MY_OBJ_DIR)/proprietary.code.check.stamp

PROPRIETARY_SOURCE :=
PROPRIETARY_SOURCE += mk/Makefile
PROPRIETARY_SOURCE += mk/blktap.spec.in
PROPRIETARY_SOURCE += drivers/block-vhd.c
PROPRIETARY_SOURCE += drivers/io-optimize.c
PROPRIETARY_SOURCE += drivers/io-optimize.h
PROPRIETARY_SOURCE += drivers/vhd-coalesce.c

PROPRIETARY_C += $(filter drivers/%.c,$(PROPRIETARY_SOURCE))
SOURCES += $(patsubst drivers/%.c,$(RPM_SOURCESDIR)/%.o,$(PROPRIETARY_C))

HG_EXCLUDE := $(PROPRIETARY_SOURCE:%=-X "%")

$(MY_OBJ_DIR)/proprietary.code.check.stamp:
	@for i in $(filter-out mk/%,$(PROPRIETARY_SOURCE)) ; do \
	    echo "Checking $${i} for proprietary code marker" ; \
	    if ! grep -q "XenSource proprietary code." $(REPO)/$${i} ; then \
	        echo "ERROR: source code marker not found in $${i}" ; \
	        exit 1 ; \
	    fi ; \
	done
	touch $@

$(RPM_SOURCESDIR)/%.o:
	make -C $(REPO)/drivers/ USE_SYSTEM_LIBRARIES=y XEN_ROOT=$(XEN_ROOT) $*.o
	mv $(REPO)/drivers/$*.o $(RPM_SOURCESDIR)/$*.o

$(RPM_SOURCESDIR)/blktap-$(BLKTAP_VERSION).tar.bz2: $(MY_OBJ_DIR)/proprietary.code.check.stamp \
	                                            $(RPM_SOURCESDIR)/.dirstamp $(REPO)
	cd $(REPO) && hg archive -t tbz2 $(HG_EXCLUDE) $@
	@if tar xjOf $@ | grep -q "XenSource proprietary code." ; then \
	    echo "ERROR: $@ contains XenSource proprietary code!"; \
	    echo "       Saved as $@.tainted" ; \
	    mv $@ $@.tainted ; \
	    /bin/false ; \
	fi

$(RPM_SOURCESDIR)/blktap-development.patch: $(MY_OBJ_DIR)/proprietary.code.check.stamp \
                                            $(RPM_SOURCESDIR)/.dirstamp $(REPO)
	cd $(REPO) && hg diff $(HG_EXCLUDE) > $@
	@if grep -q "XenSource proprietary code." $@ ; then \
	    echo "ERROR: $@ contains XenSource proprietary code!"; \
	    echo "       Saved as $@.tainted" ; \
	    mv $@ $@.tainted ; \
	    /bin/false ; \
	fi

$(RPM_SPECSDIR)/blktap.spec: blktap.spec.in $(RPM_SPECSDIR)/.dirstamp
# Use a temporary file because sed in rhel3 doesn't understand "\n"
	echo "$(CHANGELOG_HEADER)" > changelog.tmp
	echo "$(CHANGELOG_MESSAGE)" >> changelog.tmp
	echo "" >> changelog.tmp
	sed -e s\,@REPO_VERSION@,$(REPO_VERSION),g \
	    -e s\,@BLKTAP_VERSION@,$(BLKTAP_VERSION),g\
	    -e s\,@BLKTAP_RELEASE@,$(BLKTAP_RELEASE),g\
	    -e s\,@XEN_ROOT@,$(XEN_ROOT),g\
	    -e \\,%changelog,rchangelog.tmp        \
	    < blktap.spec.in                       \
	    > $(RPM_SPECSDIR)/blktap.spec
	-rm changelog.tmp

$(RPM_BUILD_COOKIE): $(RPM_DIRECTORIES) $(SOURCES)
	$(RPMBUILD) --target i686 -ba $(RPM_SPECSDIR)/blktap.spec
	@touch $@

$(MY_SOURCES)/MANIFEST: $(MY_SOURCES_DIRSTAMP)
	echo "blktap gpl+bsd+proprietary file $(MY_OUTPUT_DIR)/SRPMS/blktap-$(BLKTAP_VERSION)-$(BLKTAP_RELEASE).src.rpm" >$@
