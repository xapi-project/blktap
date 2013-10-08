USE_BRANDING    := yes
IMPORT_BRANDING := yes

include $(B_BASE)/common.mk
include $(B_BASE)/rpmbuild.mk

STAMP_CHECKOUT = $(MY_OBJ_DIR)/.stamp-checkout
STAMP_RPMBUILD = $(MY_OBJ_DIR)/.stamp-rpmbuild

-include $(MY_OUTPUT_DIR)/blktap.inc

BLKTAP_GIT      = git --git-dir $(BLKTAP_GITREPO)
BLKTAP_GITREPO	= $(call git_loc,blktap)/.git
BLKTAP_CSET	= $(shell $(call __git_repo_cset_number,$(call git_loc,blktap)))
BLKTAP_RELEASE  = xs$(PRODUCT_VERSION)
BLKTAP_SRCTREE  = $(MY_OBJ_DIR)/blktap
BLKTAP_CFGSTAT  = $(BLKTAP_SRCTREE)/config.status
BLKTAP_RPMSPEC  = $(RPM_SPECSDIR)/blktap.spec
BLKTAP_SRPMOUT  = $(MY_OUTPUT_DIR)/SRPMS/$(BLKTAP_PKGNAME)-$(BLKTAP_VERSION)-$(BLKTAP_RELEASE).src.rpm
BLKTAP_CHANGES  = $(MY_OBJ_DIR)/changelog.tmp

BLKTAP_PATCHES	= $(RPM_SOURCESDIR)/blktap-udev-ignore-tapdevs.patch

BLKTAP_OUTPUT  = $(MY_SOURCES)/MANIFEST
BLKTAP_OUTPUT += $(MY_SOURCES)/$(BLKTAP_SOURCES)
BLKTAP_OUTPUT += $(MY_OUTPUT_DIR)/blktap.inc

CHANGELOG_DATE    := $(shell LANG=C date +"%a %b %d %Y")
CHANGELOG_USER    := $(COMPANY_NAME) <$(COMPANY_WEBSITE)>
CHANGELOG_HEADER  := * $(CHANGELOG_DATE) $(CHANGELOG_USER) [$(BLKTAP_VERSION) $(BLKTAP_RELEASE)]
CHANGELOG_MESSAGE := - Build blktap.

BLKTAP_RPM_SOURCES  = $(BLKTAP_RPMSPEC)
BLKTAP_RPM_SOURCES += $(RPM_SOURCESDIR)/$(BLKTAP_SOURCES)
BLKTAP_RPM_SOURCES += $(BLKTAP_PATCHES)

.PHONY: build clean

build: $(STAMP_RPMBUILD) $(BLKTAP_OUTPUT)
	@ :

rpm-sources: $(BLKTAP_RPM_SOURCES)

$(MY_OUTPUT_DIR)/blktap.inc: blktap.inc.in $(BLKTAP_CFGSTAT)
	$(BLKTAP_CFGSTAT) --file $@:$<

$(BLKTAP_CFGSTAT): $(BLKTAP_SRCTREE)/version.m4 $(BLKTAP_SRCTREE)/EXTRAVERSION
	cd $(BLKTAP_SRCTREE) && sh autogen.sh
	cd $(BLKTAP_SRCTREE) && ./configure
	touch $@

# Patch dist target to identify as blktap-%{version}.%{extraversion}.tar.bz2
$(BLKTAP_SRCTREE)/version.m4: version.m4 $(STAMP_CHECKOUT)
	cp $< $@

$(BLKTAP_SRCTREE)/EXTRAVERSION: $(STAMP_CHECKOUT)
	echo -n .xs$(BLKTAP_CSET) > $@

$(STAMP_CHECKOUT):
	rm -rf $(BLKTAP_SRCTREE)
	mkdir $(BLKTAP_SRCTREE)
	$(BLKTAP_GIT) archive --format=tar HEAD | tar xf - -C $(BLKTAP_SRCTREE)
	touch $@

$(MY_SOURCES)/MANIFEST: $(MY_SOURCES_DIRSTAMP)
	echo "blktap gpl+bsd file $(BLKTAP_SRPMOUT)" >$@

$(MY_SOURCES)/$(BLKTAP_SOURCES): $(RPM_SOURCESDIR)/$(BLKTAP_SOURCES) $(MY_SOURCES_DIRSTAMP)
	cp $< $@

$(RPM_SOURCESDIR)/$(BLKTAP_SOURCES): $(BLKTAP_CFGSTAT)
	$(MAKE) -C $(BLKTAP_SRCTREE) dist-bzip2
	cp $(BLKTAP_SRCTREE)/$(BLKTAP_SOURCES) $@

$(BLKTAP_RPMSPEC): blktap.spec.in $(BLKTAP_CHANGES) $(RPM_DIRECTORIES) $(BLKTAP_CFGSTAT)
	cat $<	| \
		sed -e 's,@BLKTAP_RELEASE@,$(BLKTAP_RELEASE),'  \
		    -e '\,%changelog,r$(BLKTAP_CHANGES)'      | \
			$(BLKTAP_CFGSTAT) --file $@:-

$(BLKTAP_CHANGES):
	echo "$(CHANGELOG_HEADER)"   > $@
	echo "$(CHANGELOG_MESSAGE)" >> $@

$(STAMP_RPMBUILD): $(RPM_DIRECTORIES) $(BLKTAP_RPM_SOURCES)
	$(RPMBUILD) --target $(DOMAIN0_ARCH_OPTIMIZED) -ba $(BLKTAP_RPMSPEC)
	touch $@

$(BLKTAP_PATCHES): $(RPM_SOURCESDIR)/%: %
	cp $< $@

.PHONY: clean
clean: 
	rm -rf $(RPM_SOURCESDIR)
	rm -rf $(RPM_BUILDDIR)
	rm -f $(BLKTAP_RPM_SOURCES)
	rm -f $(STAMP_RPMBUILD)
	rm -f $(BLKTAP_SRCTREE)/EXTRAVERSION
