
SM_DRIVERS := File
SM_DRIVERS += NFS
SM_DRIVERS += EXT
SM_DRIVERS += ISCSI
SM_DRIVERS += Dummy
SM_DRIVERS += udev
SM_DRIVERS += ISO
SM_DRIVERS += HBA
SM_DRIVERS += RawHBA
SM_DRIVERS += LVHD
SM_DRIVERS += LVHDoISCSI
SM_DRIVERS += LVHDoHBA
SM_DRIVERS += OCFS
SM_DRIVERS += OCFSoISCSI
SM_DRIVERS += OCFSoHBA
SM_DRIVERS += SHM
SM_DRIVERS += SMB
SM_DRIVERS += LVHDoFCoE

SM_LIBS := SR
SM_LIBS += SRCommand
SM_LIBS += VDI
SM_LIBS += cleanup
SM_LIBS += lvutil
SM_LIBS += lvmcache
SM_LIBS += util
SM_LIBS += verifyVHDsOnSR
SM_LIBS += scsiutil
SM_LIBS += scsi_host_rescan
SM_LIBS += vhdutil
SM_LIBS += lvhdutil
SM_LIBS += xs_errors
SM_LIBS += nfs
SM_LIBS += devscan
SM_LIBS += sysdevice
SM_LIBS += iscsilib
SM_LIBS += mpath_dmp
SM_LIBS += mpath_null
SM_LIBS += mpath_cli
SM_LIBS += mpathutil
SM_LIBS += LUNperVDI
SM_LIBS += mpathcount
SM_LIBS += refcounter
SM_LIBS += journaler
SM_LIBS += fjournaler
SM_LIBS += lock
SM_LIBS += flock
SM_LIBS += ipc
SM_LIBS += srmetadata
SM_LIBS += metadata
SM_LIBS += lvmanager
SM_LIBS += blktap2
SM_LIBS += mpp_mpathutil
SM_LIBS += mpp_luncheck
SM_LIBS += updatempppathd
SM_LIBS += lcache
SM_LIBS += resetvdis
SM_LIBS += B_util
SM_LIBS += wwid_conf
SM_LIBS += trim_util
SM_LIBS += pluginutil
SM_LIBS += fcoelib

UDEV_RULES = 39-multipath 40-multipath 55-xs-mpath-scsidev 58-xapi
MPATH_DAEMON = sm-multipath
MPATH_CONF = multipath.conf
CIFS_CONF = cifs.conf
SMLOG_CONF = SMlog

SM_XML := XE_SR_ERRORCODES

SM_DEST := /opt/xensource/sm/
DEBUG_DEST := /opt/xensource/debug/
BIN_DEST := /opt/xensource/bin/
MASTER_SCRIPT_DEST := /etc/xensource/master.d/
PLUGIN_SCRIPT_DEST := /etc/xapi.d/plugins/
LIBEXEC := /opt/xensource/libexec/
UDEV_RULES_DIR := /etc/udev/rules.d/
UDEV_SCRIPTS_DIR := /etc/udev/scripts/
SYSTEMD_SERVICE_DIR := /usr/lib/systemd/system/
INIT_DIR := /etc/rc.d/init.d/
MPATH_CONF_DIR := /etc/multipath.xenserver/
MODPROBE_DIR := /etc/modprobe.d/
EXTENSION_SCRIPT_DEST := /etc/xapi.d/extensions/
LOGROTATE_DIR := /etc/logrotate.d/

SM_STAGING := $(DESTDIR)
SM_STAMP := $(MY_OBJ_DIR)/.staging_stamp

SM_PY_FILES = $(foreach LIB, $(SM_LIBS), drivers/$(LIB).py) $(foreach DRIVER, $(SM_DRIVERS), drivers/$(DRIVER)SR.py)

.PHONY: build
build:
	make -C dcopy 
	make -C mpathroot

.PHONY: precommit
precommit: build
	@ QUIT=0; \
	CHANGED=$$(git status --porcelain $(SM_PY_FILES) | awk '{print $$2}'); \
	for i in $$CHANGED; do \
		echo Checking $${i} ...; \
		PYTHONPATH=./snapwatchd:./drivers:$$PYTHONPATH pylint --rcfile=tests/pylintrc $${i}; \
		[ $$? -ne 0 ] && QUIT=1 ; \
	done; \
	if [ $$QUIT -ne 0 ]; then \
		exit 1; \
	fi; \
	echo "Precommit succeeded with no outstanding issues found."


.PHONY: precheck
precheck: build
	@ QUIT=0; \
	for i in $(SM_PY_FILES); do \
		echo Checking $${i} ...; \
		PYTHONPATH=./snapwatchd:./drivers:$$PYTHONPATH pylint --rcfile=tests/pylintrc $${i}; \
		[ $$? -ne 0 ] && QUIT=1 ; \
	done; \
	if [ $$QUIT -ne 0 ]; then \
		exit 1; \
	fi; \
	echo "Precheck succeeded with no outstanding issues found."

.PHONY: install
install: precheck
	mkdir -p $(SM_STAGING)
	$(call mkdir_clean,$(SM_STAGING))
	mkdir -p $(SM_STAGING)$(SM_DEST)
	mkdir -p $(SM_STAGING)$(UDEV_RULES_DIR)
	mkdir -p $(SM_STAGING)$(UDEV_SCRIPTS_DIR)
	mkdir -p $(SM_STAGING)$(INIT_DIR)
	mkdir -p $(SM_STAGING)$(SYSTEMD_SERVICE_DIR)
	mkdir -p $(SM_STAGING)$(MPATH_CONF_DIR)
	mkdir -p $(SM_STAGING)$(MODPROBE_DIR)
	mkdir -p $(SM_STAGING)$(LOGROTATE_DIR)
	mkdir -p $(SM_STAGING)$(DEBUG_DEST)
	mkdir -p $(SM_STAGING)$(BIN_DEST)
	mkdir -p $(SM_STAGING)$(MASTER_SCRIPT_DEST)
	mkdir -p $(SM_STAGING)$(PLUGIN_SCRIPT_DEST)
	mkdir -p $(SM_STAGING)$(EXTENSION_SCRIPT_DEST)
	mkdir -p $(SM_STAGING)/sbin
	for i in $(SM_PY_FILES); do \
	  install -m 755 $$i $(SM_STAGING)$(SM_DEST); \
	done
	install -m 644 multipath/$(MPATH_CONF) \
	  $(SM_STAGING)/$(MPATH_CONF_DIR)
	install -m 755 multipath/$(MPATH_DAEMON) \
	  $(SM_STAGING)/$(INIT_DIR)
	install -m 644 etc/modprobe.d/$(CIFS_CONF) \
	  $(SM_STAGING)/$(MODPROBE_DIR)
	install -m 644 etc/logrotate.d/$(SMLOG_CONF) \
	  $(SM_STAGING)/$(LOGROTATE_DIR)
	install -m 644 drivers/updatempppathd.service \
	  $(SM_STAGING)/$(SYSTEMD_SERVICE_DIR)
	install -m 644 etc/make-dummy-sr.service \
	  $(SM_STAGING)/$(SYSTEMD_SERVICE_DIR)
	install -m 644 snapwatchd/snapwatchd.service \
	  $(SM_STAGING)/$(SYSTEMD_SERVICE_DIR)
	for i in $(UDEV_RULES); do \
	  install -m 644 udev/$$i.rules \
	    $(SM_STAGING)$(UDEV_RULES_DIR); done
	for i in $(SM_XML); do \
	  install -m 755 drivers/$$i.xml \
	    $(SM_STAGING)$(SM_DEST); done
	cd $(SM_STAGING)$(SM_DEST) && for i in $(SM_DRIVERS); do \
	  ln -sf $$i"SR.py" $$i"SR"; \
	done
	rm $(SM_STAGING)$(SM_DEST)/SHMSR
	cd $(SM_STAGING)$(SM_DEST) && rm -f LVHDSR && ln -sf LVHDSR.py LVMSR
	cd $(SM_STAGING)$(SM_DEST) && rm -f LVHDoISCSISR && ln -sf LVHDoISCSISR.py LVMoISCSISR
	cd $(SM_STAGING)$(SM_DEST) && rm -f LVHDoHBASR && ln -sf LVHDoHBASR.py LVMoHBASR
	cd $(SM_STAGING)$(SM_DEST) && rm -f LVHDoFCoESR && ln -sf LVHDoFCoESR.py LVMoFCoESR
	cd $(SM_STAGING)$(SM_DEST) && rm -f OCFSSR
	cd $(SM_STAGING)$(SM_DEST) && rm -f OCFSoISCSISR
	cd $(SM_STAGING)$(SM_DEST) && rm -f OCFSoHBASR
	ln -sf $(SM_DEST)mpathutil.py $(SM_STAGING)/sbin/mpathutil
	install -m 755 drivers/02-vhdcleanup $(SM_STAGING)$(MASTER_SCRIPT_DEST)
	install -m 755 drivers/lvhd-thin $(SM_STAGING)$(PLUGIN_SCRIPT_DEST)
	install -m 755 drivers/on-slave $(SM_STAGING)$(PLUGIN_SCRIPT_DEST)
	install -m 755 drivers/testing-hooks $(SM_STAGING)$(PLUGIN_SCRIPT_DEST)
	install -m 755 drivers/coalesce-leaf $(SM_STAGING)$(PLUGIN_SCRIPT_DEST)
	install -m 755 drivers/nfs-on-slave $(SM_STAGING)$(PLUGIN_SCRIPT_DEST)
	install -m 755 drivers/tapdisk-pause $(SM_STAGING)$(PLUGIN_SCRIPT_DEST)
	install -m 755 drivers/vss_control $(SM_STAGING)$(PLUGIN_SCRIPT_DEST)
	install -m 755 drivers/intellicache-clean $(SM_STAGING)$(PLUGIN_SCRIPT_DEST)
	install -m 755 drivers/enable-borehamwood $(SM_STAGING)$(SM_DEST)
	install -m 755 drivers/trim $(SM_STAGING)$(PLUGIN_SCRIPT_DEST)
	install -m 755 drivers/enable_thin_lvhd_on_slave $(SM_STAGING)$(PLUGIN_SCRIPT_DEST)
	install -m 755 drivers/LVHD.enable_thin_provisioning $(SM_STAGING)$(EXTENSION_SCRIPT_DEST)
	ln -sf $(PLUGIN_SCRIPT_DEST)vss_control $(SM_STAGING)$(SM_DEST)
	install -m 755 drivers/iscsilib.py $(SM_STAGING)$(SM_DEST)
	install -m 755 drivers/fcoelib.py $(SM_STAGING)$(SM_DEST)
	mkdir -p $(SM_STAGING)$(LIBEXEC)
	install -m 755 scripts/local-device-change $(SM_STAGING)$(LIBEXEC)
	install -m 755 scripts/check-device-sharing $(SM_STAGING)$(LIBEXEC)
	$(MAKE) -C dcopy install DESTDIR=$(SM_STAGING)
	$(MAKE) -C snapwatchd install DESTDIR=$(SM_STAGING)
	$(MAKE) -C mpathroot install DESTDIR=$(SM_STAGING)
	ln -sf $(SM_DEST)blktap2.py $(SM_STAGING)$(BIN_DEST)/blktap2
	ln -sf $(SM_DEST)lcache.py $(SM_STAGING)$(BIN_DEST)tapdisk-cache-stats
	ln -sf /dev/null $(SM_STAGING)$(UDEV_RULES_DIR)/69-dm-lvm-metad.rules
	install -m 755 scripts/xs-mpath-scsidev.sh $(SM_STAGING)$(UDEV_SCRIPTS_DIR)
	install -m 755 scripts/xe-get-arrayid-lunnum $(SM_STAGING)$(BIN_DEST)
	install -m 755 scripts/xe-getarrayidentifier $(SM_STAGING)$(BIN_DEST)
	install -m 755 scripts/xe-getlunidentifier $(SM_STAGING)$(BIN_DEST)
	install -m 755 scripts/make-dummy-sr $(SM_STAGING)$(LIBEXEC)

.PHONY: clean
clean:
	rm -rf $(SM_STAGING)

