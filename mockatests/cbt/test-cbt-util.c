#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <uuid/uuid.h>

#include <cbt-util-priv.h>

extern int cbt_util_create(int , char **);
extern int cbt_util_set(int , char **);
extern int cbt_util_get(int , char **);

struct cbt_log_metadata {
	uuid_t parent;
	uuid_t child;
	int    consistent;
};

FILE *
__wrap_fopen(void)
{
	return (FILE*) mock();
}

void __real_fclose(FILE *fp);

void
__wrap_fclose(FILE *fp)
{
	check_expected_ptr(fp);
	__real_fclose(fp);
}

int
wrap_vprintf(const char *format, va_list ap)
{
	int bufsize = mock();
	char* buf = mock();

	int len = vsnprintf(buf, bufsize, format, ap);

	assert_in_range(len, 0, bufsize);

	return len;
}

int
__wrap_printf(const char *format, ...)
{
	va_list ap;
	va_start(ap, format);

	return wrap_vprintf(format, ap);
}

int
__wrap___printf_chk (int __flag, const char *format, ...)
{
	va_list ap;
	va_start(ap, format);

	return wrap_vprintf(format, ap);
}

void
test_get_command_create(void **state)
{
	struct command *cmd;

	char* requested_command = { "create" };

	cmd = get_command(requested_command);

	assert_string_equal(cmd->name, "create");
	assert_ptr_equal(cmd->func, cbt_util_create);
}

void
test_get_command_set(void **state)
{
	struct command *cmd;

	char* requested_command = { "set" };

	cmd = get_command(requested_command);

	assert_string_equal(cmd->name, "set");
	assert_ptr_equal(cmd->func, cbt_util_set);
}

void
test_get_command_get(void **state)
{
	struct command *cmd;

	char* requested_command = { "get" };

	cmd = get_command(requested_command);

	assert_string_equal(cmd->name, "get");
	assert_ptr_equal(cmd->func, cbt_util_get);
}


void test_cbt_util_get_flag(void **state)
{
	int result;
	char* args[] = { "cbt-util", "-n", "test_disk.log", "-f" };
	void *log_meta;
	char output[1024];

	log_meta = malloc(sizeof(struct cbt_log_metadata));

	((struct cbt_log_metadata*)log_meta)->consistent = 1;
	FILE *test_log = fmemopen((void*)log_meta, sizeof(struct cbt_log_metadata), "r");

	will_return(__wrap_fopen, test_log);
	expect_value(__wrap_fclose, fp, test_log);

	will_return(wrap_vprintf, 1024);
	will_return(wrap_vprintf, output);

	result = cbt_util_get(4, args);

	assert_int_equal(result, 0);
	assert_string_equal(output, "1\n");
}

const struct CMUnitTest cbt_command_tests[] = {
	cmocka_unit_test(test_get_command_create),
	cmocka_unit_test(test_get_command_set),
	cmocka_unit_test(test_get_command_get)
};

const struct CMUnitTest cbt_get_tests[] = {
	cmocka_unit_test(test_cbt_util_get_flag)
};

int main(void)
{
	return
		cmocka_run_group_tests_name("Command tests", cbt_command_tests, NULL, NULL) +
		cmocka_run_group_tests_name("Get tests", cbt_get_tests, NULL, NULL);
}
