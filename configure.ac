m4_include([version.m4])
AC_INIT([blktap], BLKTAP_VERSION)
AM_INIT_AUTOMAKE([foreign])
# AM_SILENT_RULES([yes])

AC_PROG_CC
AC_PROG_LIBTOOL
AC_PROG_INSTALL
AC_PROG_LN_S
AC_PROG_MAKE_SET
AC_CANONICAL_HOST
AC_CHECK_HEADERS([uuid/uuid.h], [], [Need uuid-dev])
AC_CHECK_HEADERS([libaio.h], [], [Need libaio-dev])

AC_CHECK_LIB([aio], [main],
             [:],
             AC_MSG_ERROR([Need libaio-dev]))

AC_CHECK_LIB([uuid], [main],
             [:],
	     AC_MSG_ERROR([Need uuid-dev]))

AM_CONDITIONAL([ENABLE_SHARED],
	       [test x$enable_shared = xyes])

# AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([
Makefile
lvm/Makefile
part/Makefile
vhd/Makefile
vhd/lib/Makefile
vhd/lib/test/Makefile
control/Makefile
drivers/Makefile
include/Makefile
])
AC_OUTPUT
