USE_BRANDING := yes
IMPORT_BRANDING := yes
IMPORT_VERSIONS := yes
REPONAME := sm

include $(B_BASE)/common.mk
include $(B_BASE)/rpmbuild.mk

-include $(MY_OBJ_DIR)/version.inc
.PHONY: $(MY_OBJ_DIR)/version.inc
$(MY_OBJ_DIR)/version.inc:
	$(version-makefile) > $@
	$(call git_cset_number,$(REPONAME)) >> $@
	echo SM_VERSION := \$$\(PLATFORM_VERSION\) >> $@
	echo SM_RELEASE := xs\$$\(CSET_NUMBER\) >> $@

SM_SPEC := sm.spec
SM_SRC := $(RPM_SOURCESDIR)/sm-$(SM_VERSION).tar.gz
SM_SRPM := sm-$(SM_VERSION)-$(SM_RELEASE).src.rpm
SM_STAMP := $(MY_OBJ_DIR)/.rpmbuild.stamp
ERRORCODES_XML := XE_SR_ERRORCODES.xml
SM_TESTS := $(MY_OUTPUT_DIR)/storage-manager-tests.tar
SM_UNIT_TESTS := $(MY_OUTPUT_DIR)/smunittests.tar
REPO := $(call git_loc,sm)
SUPPORT_PACK := $(MY_OUTPUT_DIR)/borehamwood-supp-pack.iso
BUILD := $(BUILD_NUMBER)

.PHONY: build
build: sources $(SM_STAMP) $(SM_TESTS) $(SM_UNIT_TESTS) \
	$(MY_OUTPUT_DIR)/$(ERRORCODES_XML) $(SUPPORT_PACK)

$(MY_SOURCES)/MANIFEST: $(RPM_SRPMSDIR)/$(SM_SRPM) $(MY_SOURCES_DIRSTAMP) $(RPM_BUILD_COOKIE)
	rm -f $@
	/bin/sh ./srpms-to-manifest $(COMPONENT) $(MY_OUTPUT_DIR)/SRPMS > $@

.PHONY: sources
sources: $(MY_SOURCES)/MANIFEST

.PHONY: clean
clean:
	rm -f $(SM_STAMP) $(SM_SRC) $(RPM_SPECSDIR)/$(SM_SPEC)

.SECONDARY: $(SM_SRC)
$(SM_SRC): $(SM_SOURCES)
	echo SM_VERSION $(SM_VERSION) PLATFORM_VERSION $(PLATFORM_VERSION)
	git --git-dir $(REPO)/.git archive --prefix=sm-$(SM_VERSION)/ \
		--format=tar HEAD | bzip2 > $(RPM_SOURCESDIR)/sm-$(SM_VERSION).tar.bz2

.SECONDARY: $(RPM_SPECSDIR)/%.spec
$(RPM_SPECSDIR)/%.spec: *.spec.in
	sed -e 's/@SM_VERSION@/$(SM_VERSION)/g' \
          -e 's/@SM_RELEASE@/$(SM_RELEASE)/g' \
          < $< \
          > $@

$(RPM_SRPMSDIR)/$(SM_SRPM): $(RPM_DIRECTORIES) $(RPM_SPECSDIR)/$(SM_SPEC) $(SM_SRC)
	$(RPMBUILD) -bs $(RPM_SPECSDIR)/$(SM_SPEC)

$(SM_STAMP): $(RPM_SRPMSDIR)/$(SM_SRPM)
	$(RPMBUILD) --rebuild --target $(DOMAIN0_ARCH_OPTIMIZED) $(RPM_SRPMSDIR)/$(SM_SRPM)
	mkdir -p $(MY_MAIN_PACKAGES)
	cp $(RPM_RPMSDIR)/$(DOMAIN0_ARCH_OPTIMIZED)/sm-*.rpm $(MY_MAIN_PACKAGES)
	# Deliberately omit the debuginfo RPM (sm-debuginfo-...)
	rm -f $(MY_MAIN_PACKAGES)/sm-debuginfo-*.rpm
	# sm-rawhba is packaged as supp-pack. Skip it.
	rm -f $(MY_MAIN_PACKAGES)/sm-rawhba-*.rpm
	touch $@

$(SM_TESTS): 
	rm -f $@
	tar -cf $@ -C $(REPO) tests

$(SM_UNIT_TESTS):
	rm -f $@
	tar -cf $@ -C $(REPO) tests/lvhd_test

$(MY_OUTPUT_DIR)/$(ERRORCODES_XML):
	rm -f $@
	cp $(REPO)/drivers/$(ERRORCODES_XML) $@

$(SUPPORT_PACK): $(SM_STAMP)
	python setup.py --out $(dir $@) --pdn $(PRODUCT_BRAND) --pdv $(PRODUCT_VERSION) --bld $(BUILD) --spn "borehamwood-supp-pack" --spd "XenServer RawHBA implementation" --rxv "6.1.0" $(RPM_RPMSDIR)/$(DOMAIN0_ARCH_OPTIMIZED)/sm-rawhba-*.$(DOMAIN0_ARCH_OPTIMIZED).rpm
