USE_BRANDING := yes
IMPORT_BRANDING := yes
include $(B_BASE)/common.mk
include $(B_BASE)/rpmbuild.mk

REPO_NAME= blktap
REPO=$(call hg_loc,blktap)
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
CHANGELOG_USER    := $(COMPANY_NAME) <$(COMPANY_WEBSITE)>
CHANGELOG_HEADER  := * $(CHANGELOG_DATE) $(CHANGELOG_USER) [$(BLKTAP_VERSION) $(BLKTAP_RELEASE)]
CHANGELOG_MESSAGE := - Build blktap.

SOURCES = $(RPM_SOURCESDIR)/blktap-$(BLKTAP_VERSION).tar.bz2 \
          $(RPM_SOURCESDIR)/blktap-udev-ignore-tapdevs.patch \
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

$(RPM_SOURCESDIR)/%.o:
	rm -f $(REPO)/$(dir $*).$(notdir $*).o.d $(REPO)/$*.o
	make -C $(REPO)/$(dir $*) USE_SYSTEM_LIBRARIES=y $(notdir $*.o)
	mv $(REPO)/$*.o $(RPM_SOURCESDIR)/$(notdir $*.o)

$(RPM_SOURCESDIR)/blktap-$(BLKTAP_VERSION).tar.bz2: $(RPM_SOURCESDIR)/.dirstamp $(REPO)
	cd $(REPO) && hg archive -t tbz2 $(HG_EXCLUDE) $@

$(RPM_SOURCESDIR)/%.patch: %.patch
	cp $< $@

$(RPM_SPECSDIR)/blktap.spec: blktap.spec.in $(RPM_SPECSDIR)/.dirstamp
# Use a temporary file because sed in rhel3 doesn't understand "\n"
	echo "$(CHANGELOG_HEADER)" > changelog.tmp
	echo "$(CHANGELOG_MESSAGE)" >> changelog.tmp
	echo "" >> changelog.tmp
	sed -e s\,@REPO_VERSION@,$(REPO_VERSION),g \
	    -e s\,@BLKTAP_VERSION@,$(BLKTAP_VERSION),g\
	    -e s\,@BLKTAP_RELEASE@,$(BLKTAP_RELEASE),g\
	    -e \\,%changelog,rchangelog.tmp        \
	    < blktap.spec.in                       \
	    > $(RPM_SPECSDIR)/blktap.spec
	-rm changelog.tmp

$(RPM_BUILD_COOKIE): $(RPM_DIRECTORIES) $(SOURCES)
	$(RPMBUILD) --target $(DOMAIN0_ARCH_OPTIMIZED) \
		    -ba $(RPM_SPECSDIR)/blktap.spec 
	@touch $@

$(MY_SOURCES)/MANIFEST: $(MY_SOURCES_DIRSTAMP)
	echo "blktap gpl+bsd file $(MY_OUTPUT_DIR)/SRPMS/blktap-$(BLKTAP_VERSION)-$(BLKTAP_RELEASE).src.rpm" >$@
