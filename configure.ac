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
AC_SYS_LARGEFILE
AC_CHECK_HEADERS([uuid/uuid.h], [], [Need uuid-dev])
AC_CHECK_HEADERS([libaio.h], [], [Need libaio-dev])
AC_CHECK_HEADERS([limits.h], [], [AC_MSG_ERROR([cannot find limits.h])])
AC_CHECK_HEADERS([time.h], [], [AC_MSG_ERROR([cannot find time.h])])

AC_ARG_WITH([libiconv],
	     [AS_HELP_STRING([--with-libiconv],
			     [link to libiconv])],
             [],
             [with_libiconv=check])

AC_CHECK_LIB([aio], [main],
             [:],
             AC_MSG_ERROR([Need libaio-dev]))

AC_CHECK_LIB([uuid], [main],
             [:],
	     AC_MSG_ERROR([Need uuid-dev]))

AS_IF([test x$with_libiconv != xno],
      [AC_CHECK_LIB([iconv], [main],
		    [AC_SUBST([LIBICONV], ["-liconv"])],
		    [if test x$with_libiconv == xyes; then
		       AC_MSG_FAILURE([--with-libiconv given, but test failed])
		     fi])],
      [AC_CHECK_LIB([], [iconv_open],
                    [:],
		    AC_MSG_ERROR([Missing iconv in libc]))])

AC_ARG_ENABLE([tests],
	      [AS_HELP_STRING([--enable-tests],
			      [build test programs])],
	      [],
	      [enable_tests=no])

AM_CONDITIONAL([ENABLE_PART],
	       [case "${host_os}" in
		      linux-*) true ;;
		      *) false ;;
		esac])

AM_CONDITIONAL([ENABLE_VHDIO],
	       [test x$enable_shared = xyes &&
	       	case "${host_os}" in
		      linux-gnu) true ;;
		      *) false ;;
		esac])

AM_CONDITIONAL([ENABLE_TESTS],
	       [test x$enable_tests = xyes])

AC_CHECK_FUNCS([eventfd])



# AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_HEADERS([config.h])
AC_DEFINE(_BLKTAP, 1,
		  Indicates whether this is an internal or external compilation.)
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
tapback/Makefile
thin/Makefile
])
AC_OUTPUT
