PROG_ROOT = ../..

AM_CFLAGS = -g -DTEST_FUNCTION -I${PROG_ROOT}/ -I${PROG_ROOT}/${PBS_MACH} --coverage

lib_LTLIBRARIES = libparse_jobid.la

AM_LDFLAGS = @CHECK_LIBS@ ${lib_LTLIBRARIES}

check_PROGRAMS = test_parse_jobid

libparse_jobid_la_SOURCES = scaffolding.c ${PROG_ROOT}/parse_jobid.c
libparse_jobid_la_LDFLAGS = @CHECK_LIBS@ -shared -lgcov

test_parse_jobid_SOURCES = test_parse_jobid.c

check_SCRIPTS = ${PROG_ROOT}/../../test/coverage_run.sh

TESTS = ${check_PROGRAMS} ${check_SCRIPTS}

CLEANFILES = *.gcno *.gcda *.gcov core *.lo
