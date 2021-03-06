
#include <libws.h>
#include <libws_log.h>
#include <inttypes.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <assert.h>
#include "jansson.h"
#include "cargo/cargo.h"
#include "libws_test_helpers.h"

#ifdef _WIN32
#define snprintf _snprintf
#endif

typedef enum libws_autobahn_state_e
{
	LIBWS_AUTOBAHN_STATE_TEST = 0,
	LIBWS_AUTOBAHN_STATE_REPORT = 1,
	LIBWS_AUTOBAHN_STATE_TESTINFO = 2,
	LIBWS_AUTOBAHN_STATE_TESTSTATUS = 3,
	LIBWS_AUTOBAHN_STATE_COUNT = 4,
} libws_autobahn_state_t;

typedef struct libws_autobahn_args_s
{
	int ssl;
	int port;
	int range[2];
	size_t range_count;
	char *agentname;
	size_t agentname_count;
	char *server;
	int reports;
	int help;
	int debug;
	int nocolor;
	int all;
	int maxtime;
	int fulldata;
	int compact;
	int quiet;
	char *config;

	int skiprange[2];
	size_t skiprange_count;

	int *skip;
	size_t skip_count;

	int *tests;
	size_t test_count;

	char **extras;
	size_t extra_count;
} libws_autobahn_args_t;

libws_autobahn_state_t state;
libws_autobahn_args_t args;
int server_case_count = 0;
int current_case = -1;
int global_return = 0;

void draw_line()
{
	int i;

	for (i = 0; i < 80; i++)
		printf("-");

	printf("\n");
}

void print_linebreak(char *str, int width)
{
	char *s = strdup(str);
	char *start = s;
	char *prev = s;
	char *p = NULL;

	if (!s)
		return;

	p = strpbrk(s, " ");
	while (p != NULL)
	{
		if ((p - start) > width)
		{
			*prev = '\n';
			start = p;
		}

		prev = p;
		p = strpbrk(p + 1, " ");
	}

	libws_test_STATUS("%s", s);
	free(s);
}

void parse_test_info(char *msg)
{
	char headline[128];
	json_error_t error;
	json_t *json = NULL;
	char *id = NULL;
	char *description = NULL;

	if (!(json = json_loads(msg, 0, &error)))
	{
		fprintf(stderr, "Failed to load test info json: %s\n", error.text);
		return;
	}

	if (json_unpack_ex(json, &error, 0,
		"{"
			"s:s"
			"s:s"
		"}",
		"id", &id,
		"description", &description))
	{
		fprintf(stderr, "Failed to parse test info: %s\n", error.text);
		json_decref(json);
		return;
	}

	snprintf(headline, sizeof(headline), "[%*d] - %*s", 3, current_case, 6, id);

	if (args.compact)
	{
		libws_test_printf(stdout, MAGNETA, "%*s: ", 10, headline);
	}
	else
	{
		libws_test_HEADLINE(headline);
		print_linebreak(description, 80);
	}
}

void parse_test_status(char *msg)
{
	json_error_t error;
	json_t *json = NULL;
	char *behavior = NULL;

	if (!(json = json_loads(msg, 0, &error)))
	{
		fprintf(stderr, "Failed to load test info json: %s\n", error.text);
		return;
	}

	if (json_unpack_ex(json, &error, 0,
		"{"
			"s:s"
		"}",
		"behavior", &behavior))
	{
		fprintf(stderr, "Failed to parse test info: %s\n", error.text);
		json_decref(json);
		return;
	}

	if (!strcmp(behavior, "OK") || !strcmp(behavior, "INFORMATIONAL"))
	{
		if (args.compact)
		{
			libws_test_printf(stderr, BRIGHT, "[");
			libws_test_printf(stderr, GREEN, "SUCCESS");
			libws_test_printf(stderr, BRIGHT, "] ");
		}
		else
		{
			libws_test_SUCCESS("");
		}
	}
	else
	{
		if (args.compact)
		{
			libws_test_printf(stderr, BRIGHT, "[");
			libws_test_printf(stderr, RED, "FAILURE");
			libws_test_printf(stderr, BRIGHT, "] ");
		}
		else
		{
			libws_test_FAILURE("");
		}
		global_return = -1;
	}
}

void onmsg(ws_t ws, char *msg, uint64_t len, int binary, void *arg)
{
	switch (state)
	{
		case LIBWS_AUTOBAHN_STATE_REPORT:
		{
			printf("Got report! %s\n", msg);
			break;
		}
		case LIBWS_AUTOBAHN_STATE_TESTINFO:
		{
			parse_test_info(msg);
			break;
		}
		case LIBWS_AUTOBAHN_STATE_TESTSTATUS:
		{
			parse_test_status(msg);
			break;
		}
		case LIBWS_AUTOBAHN_STATE_COUNT: 
		{
			server_case_count = atoi(msg);
			break;
		}
		default:
		case LIBWS_AUTOBAHN_STATE_TEST:
		{
			if (!args.quiet)
			{
				if (args.fulldata && !binary)
				{
					printf("%s (%"PRIu64" bytes) ", msg, len);
				}
				else
				{
					printf("%*"PRIu64" bytes (%s)",
						args.compact ? 10 : 0, len,
						binary ? "binary" : "text");
				}

				if (!args.compact)
					printf("\n");
			}

			// Echo the message to the server.
			ws_send_msg_ex(ws, msg, len, binary);
			break;
		}
	}
}

void onping(ws_t ws, char *payload, uint64_t len, int binary, void *arg)
{
	ws_default_onping_cb(ws, payload, len, binary, arg);
	printf("Ping! (%"PRIu64" byte payload)\n", len);
}

void onclose(ws_t ws, ws_close_status_t status,
			const char *reason, size_t reason_len, void *arg)
{
	if (state == LIBWS_AUTOBAHN_STATE_TEST && !args.compact)
		printf("Close status: %u. %s\n", (uint16_t)status, reason);

	ws_base_quit(ws_get_base(ws), 1);
}

void onconnect(ws_t ws, void *arg)
{
	if ((state == LIBWS_AUTOBAHN_STATE_TEST) && !args.compact)
		printf("Connected!\n");
}

int do_connect(char *url)
{
	int ret = 0;
	ws_base_t base = NULL;
	ws_t ws = NULL;
	struct timeval max_test_run_time = {args.maxtime, 0}; // 20 seconds.

	if (ws_global_init(&base))
	{
		fprintf(stderr, "Failed to init global state.\n");
		return -1;
	}

	if (ws_init(&ws, base))
	{
		fprintf(stderr, "Failed to init websocket state.\n");
		ret = -1;
		goto fail;
	}

	ws_set_onmsg_cb(ws, onmsg, NULL);
	ws_set_onconnect_cb(ws, onconnect, NULL);
	ws_set_onclose_cb(ws, onclose, NULL);
	ws_set_onping_cb(ws, onping, NULL);

	if (args.ssl)
	{
		ws_set_ssl_state(ws, LIBWS_SSL_SELFSIGNED);
	}

	if (ws_connect(ws, args.server, args.port, url))
	{
		ret = -1;
		goto fail;
	}

	ws_base_quit_delay(base, 1, &max_test_run_time);
	ws_base_service_blocking(base);

fail:
	ws_destroy(&ws);
	ws_global_destroy(&base);

	return ret;
}

int update_reports()
{
	char url[1024];
	int ret = 0;

	printf("Updating reports!\n");

	snprintf(url, sizeof(url), 
			"updateReports?agent=%s", args.agentname);
	state = LIBWS_AUTOBAHN_STATE_REPORT;

	if (do_connect(url))
	{
		ret = -1;
	}

	return ret;
}

int run_case(int testcase)
{
	char url[1024];
	int ret = 0;

	current_case = testcase;

	// Get test info.
	snprintf(url, sizeof(url), "getCaseInfo?case=%d&agent=%s",
			testcase, args.agentname);
	state = LIBWS_AUTOBAHN_STATE_TESTINFO;

	if (do_connect(url))
	{
		ret = -1;
		goto fail;
	}

	// Run test.
	snprintf(url, sizeof(url), "runCase?case=%d&agent=%s",
			testcase, args.agentname);
	state = LIBWS_AUTOBAHN_STATE_TEST;

	if (do_connect(url))
	{
		ret = -1;
		goto fail;
	}

	// Print status.
	snprintf(url, sizeof(url), "getCaseStatus?case=%d&agent=%s",
			testcase, args.agentname);
	state = LIBWS_AUTOBAHN_STATE_TESTSTATUS;

	if (do_connect(url))
	{
		ret = -1;
		goto fail;
	}

fail:
	printf("\n");

	return ret;
}

int get_case_count()
{
	char url[1024];
	int ret = 0;

	snprintf(url, sizeof(url), "getCaseCount");
	state = LIBWS_AUTOBAHN_STATE_COUNT;

	if (do_connect(url))
	{
		ret = -1;
	}
	else
	{
		ret = server_case_count;
	}

	return ret;
}

int skip_case(int testcase)
{
	size_t j;

	for (j = 0; j < args.skip_count; j++)
	{
		if (args.skip[j] == testcase)
		{
			return 1;
		}
	}

	return 0;
}

int int_compare(const void * a, const void * b)
{
   return (*(int*)a - *(int*)b);
}

int combine_range_and_values(int max_case,
	const int *range, size_t range_count,
	int **values, size_t *value_count)
{
	size_t i;
	size_t j;
	int start = 0;
	int stop = 0;
	int more_count = 0;
	int tmp_count = 0;
	int *tmp = NULL;
	int *_values;
	assert(range);
	assert(values);

	_values = *values;

	start = range[0];
	stop = (range_count == 2) ? range[1] : max_case;

	if (start <= 0)
	{
		fprintf(stderr, "Range start must be positive integer!\n");
		return -1;
	}

	// We will include the tests specified using --tests as well
	// count how many of those are outside the range.
	for (i = 0; i < *value_count; i++)
	{
		if ((_values[i] < (int)start) || (_values[i] > (int)stop))
		{
			more_count++;
		}
	}

	// Make sure we have a valid range.
	tmp_count = (stop - start) + 1;

	if (tmp_count <= 0)
	{
		fprintf(stderr, "Invalid range specified! "
			"Start needs to be larger than stop\n");
		return -1;
	}

	// Include the extra tests in the count.
	tmp_count += more_count;

	if (!(tmp = malloc(sizeof(int) * tmp_count)))
	{
		fprintf(stderr, "Out of memory\n");
		exit(1);
	}

	for (i = 0, j = start; j <= (size_t)stop; i++)
	{
		tmp[i] = j++;
	}

	// Copy the old tests into the args.
	for (j = 0; j < *value_count; j++)
	{
		if ((_values[j] < start) || (_values[j] > stop))
		{
			tmp[i] = _values[j];
			i++;
		}
	}

	// Finalize the list by sorting it.
	*value_count = tmp_count;

	if (*values)
	{
		free(*values);
		*values = NULL;
	}

	*values = tmp;
	qsort(*values, *value_count, sizeof(int), int_compare);

	return 0;
}

int run_cases()
{
	size_t i;
	int max_case = 0;
	if ((max_case = get_case_count()) < 0)
	{
		fprintf(stderr, "Failed to get case count!\n");
		return -1;
	}

	// Setup "all" or a range of tests.
	// (These overwrite the normal tests).
	if (args.all)
	{
		args.test_count = max_case;

		if (!(args.tests = realloc(args.tests, sizeof(int) * args.test_count)))
		{
			fprintf(stderr, "Out of memory\n");
			exit(1);
		}

		for (i = 0; i < args.test_count; i++)
		{
			args.tests[i] = i + 1;
		}
	}
	else if (args.range_count)
	{
		if (combine_range_and_values(max_case, 
			args.range, args.range_count, 
			&args.tests, &args.test_count))
		{
			return -1;
		}
	}

	if (args.skiprange_count)
	{
		if (combine_range_and_values(max_case,
			args.skiprange, args.skiprange_count,
			&args.skip, &args.skip_count))
		{
			return -1;
		}
	}

	// Print the cases we will run.
	draw_line();
	{
		int len = 0;

		printf("Running test cases:\n");

		for (i = 0; i < args.test_count; i++)
		{
			if (skip_case(args.tests[i])) continue;

			len += printf("%d%s", args.tests[i],
				((i + 1) == args.test_count) ? "" : ", ");

			if (len >= 78)
			{
				len = 0;
				printf("\n");
			}
		}

		printf("\n");
	}
	draw_line();

	// Finally run the actual test cases.
	for (i = 0; i < args.test_count; i++)
	{
		if (skip_case(args.tests[i])) continue;

		if (run_case(args.tests[i]))
		{
			return -1;
		}
	}

	update_reports();

	return 0;
}

void print_range(int range_count, int *range)
{
	if (range_count == 1)
	{
		printf("%d to MAX\n", range[0]);
	}
	else if (range_count == 2)
	{
		printf("%d to %d\n", range[0], range[1]);
	}
	else
	{
		printf("-\n");
	}
}

int load_config_int_array(json_t *array, const char *name,
	int **target, size_t *target_count)
{
	int ret = 0;
	size_t i;

	if (array)
	{
		if (!json_is_array(array))
		{
			fprintf(stderr, "Error parsing JSON: \"%s\" is not "
					"an array.\n", name);
			ret = -1;
			goto fail;
		}
		else
		{
			json_t *val = NULL;
			*target_count = json_array_size(array);

			if (!(*target = realloc(*target, 
					sizeof(int) * (*target_count))))
			{
				fprintf(stderr, "Out of memory!\n");
				exit(1);
			}

			for (i = 0; i < *target_count; i++)
			{
				val = json_array_get(array, i);

				if (!json_is_integer(val))
				{
					fprintf(stderr, "Item at index %lu in \"%s\" is not "
						"an integer!\n", i, name);
					ret = -1;
					goto fail;
				}

				(*target)[i] = (int)json_integer_value(val);
			}
		}
	}

fail:
	return ret;
}

int load_config_int_range(json_t *range, char *name, 
				int *target, size_t *target_count)
{
	json_error_t error;

	if (!range)
		return 0;

	if (json_is_array(range))
	{
		if (json_unpack_ex(range, &error, 0,
			"[i,i]",
			&target[0],
			&target[1]))
		{
			fprintf(stderr, "Failed to parse %s: %s\n", name, error.text);
			return -1;
		}

		*target_count = 2;
	}
	else
	{
		fprintf(stderr, "\"%s\" is not an array.\n", name);
		return -1;
	}

	return 0;
}

int load_config(const char *path)
{
	int ret = 0;
	json_t *json = NULL;
	json_t *tests = NULL;
	json_t *skips = NULL;
	json_t *skiprange = NULL;
	json_t *testrange = NULL;
	char *server = NULL;
	char *agentname = NULL;
	json_error_t error;

	if (!(json = json_load_file(path, 0, &error)))
	{
		fprintf(stderr, "Error loading config: %s\n", error.text);
		return -1;
	}

	if (json_unpack_ex(json, &error, 0,
		"{"
			"s?:s" // server
			"s?:i" // port
			"s?:i" // maxtime
			"s?:i" // ssl
			"s?:i" // debug
			"s?:i" // nocolor
			"s?:i" // quiet
			"s?:o" // tests
			"s?:o" // skips
			"s?:o" // testrange
			"s?:o" // skiprange
		"}",
		"server", &server,
		"port", &args.port,
		"maxtime", &args.maxtime,
		"agent", &agentname,
		"ssl", &args.ssl,
		"debug", &args.debug,
		"nocolor", &args.nocolor,
		"quiet", &args.quiet,
		"tests", &tests,
		"skips", &skips,
		"testrange", &testrange,
		"skiprange", &skiprange))
	{
		fprintf(stderr, "Failed to unpack config: %s\n", error.text);
		return -1;
	}

	if (server)
		args.server = strdup(server);

	if (agentname)
		args.agentname = strdup(agentname);

	if (load_config_int_array(tests, "tests", &args.tests, &args.test_count))
		goto fail;

	if (load_config_int_array(skips, "skips", &args.skip, &args.skip_count))
		goto fail;

	if (load_config_int_range(skiprange, "test range", 
		args.range, &args.range_count))
	{
		goto fail;
	}

	if (load_config_int_range(skiprange, "skip range", 
		args.skiprange, &args.skiprange_count))
	{
		goto fail;
	}

fail:
	json_decref(json);
	return ret;
}

int main(int argc, char **argv)
{
	int ret = 0;
	cargo_t cargo;

	printf("\nlibws AutobahnTestSuite client (C) Joakim Soderberg 2014\n\n");
	printf("  jansson v%s\n\n", JANSSON_VERSION);

	// Parse command line arguments.
	{
		args.ssl = 0;
		args.port = 9001;
		args.agentname = "libws";
		args.server = NULL;
		args.reports = 0;
		args.help = 0;
		args.debug = 0;
		args.all = 0;
		args.maxtime = 30;
		args.fulldata = 0;

		cargo_init(&cargo, 32, argv[0],
			"An AutobahnTestSuite client that can be "
			"used to verify websocket compliance.");

		ret |= cargo_add(cargo, "--help", &args.help, CARGO_BOOL,
					"Show this help.");
		cargo_add_alias(cargo, "--help", "-h");

		ret |= cargo_add(cargo, "--debug", &args.debug, CARGO_BOOL,
					"Show websocket debug output.");
		cargo_add_alias(cargo, "--debug", "-d");

		ret |= cargo_add(cargo, "--fulldata", &args.fulldata, CARGO_BOOL,
					"Output the full test data.");

		ret |= cargo_add(cargo, "--compact", &args.compact, CARGO_BOOL,
					"Use a compact output format.");

		ret |= cargo_add(cargo, "--ssl", &args.ssl, CARGO_BOOL,
					"Use SSL for the websocket connection.");

		ret |= cargo_add(cargo, "--nocolor", &args.nocolor, CARGO_BOOL,
					"Turn off fancy color output.");

		ret |= cargo_add(cargo, "--port", &args.port, CARGO_INT,
					"The websocket port to use.");
		cargo_add_alias(cargo, "--port", "-p");

		ret |= cargo_add(cargo, "--maxtime", &args.maxtime, CARGO_INT,
					"The max time a test case is allowed to run.");

		ret |= cargo_add(cargo, "--agent", &args.agentname, CARGO_STRING,
					"The name of the user agent. Default is 'libws'.");

		ret |= cargo_add(cargo, "--reports", &args.reports, CARGO_BOOL,
				"Tell the server to update the Autobahn Test Suites "
				"reports manually.");
		cargo_add_alias(cargo, "--reports", "-r");

		ret |= cargo_add(cargo, "--config", &args.config, CARGO_STRING,
					"Configuration file containing the tests to run.");
		cargo_add_alias(cargo, "--config", "-c");

		ret |= cargo_addv_alloc(cargo, "--skip", (void **)&args.skip, 
				&args.skip_count, CARGO_NARGS_ONE_OR_MORE, CARGO_INT,
				"");
		cargo_add_alias(cargo, "--skip", "-s");

		ret |= cargo_addv(cargo, "--skiprange", (void **)&args.skiprange, 
				&args.skiprange_count, 2, CARGO_INT,
				"Skip this range of tests. Specify start and stop test case.");

		ret |= cargo_addv(cargo, "--testrange", (void **)&args.range, 
				&args.range_count, 2, CARGO_INT,
				"Adds a range of tests. Specify start and stop test case. "
				"These will be appended to the tests specified with --tests");

		ret |= cargo_add(cargo, "--all", &args.all, CARGO_BOOL,
					"Run all tests.");
		cargo_add_alias(cargo, "--all", "-a");

		ret |= cargo_addv_alloc(cargo, "--tests", (void **)&args.tests, 
				&args.test_count, CARGO_NARGS_ONE_OR_MORE, CARGO_INT,
				"A list of tests to run.");
		cargo_add_alias(cargo, "--tests", "-t");

		ret |= cargo_add(cargo, "--quiet", &args.quiet, CARGO_BOOL,
					"Don't output anything relating to the data in the messages."
					"This can be good when running the tests that spam a lot of data.");

		if (ret != 0)
		{
			fprintf(stderr, "Failed to add argument\n");
			return -1;
		}

		if (cargo_parse(cargo, 1, argc, argv))
		{
			cargo_print_usage(cargo);
			fprintf(stderr, "Error parsing!\n");
			ret = -1;
			goto done;
		}

		args.extras = cargo_get_args(cargo, &args.extra_count);
	}

	// Verify and print settings.
	if (args.nocolor)
	{
		libws_test_nocolor(1);
	}

	if (args.help)
	{
		cargo_print_usage(cargo);
		return -1;
	}

	if (args.debug)
	{
		ws_set_log_cb(ws_default_log_cb);
		ws_set_log_level(-1);
	}

	if (args.config)
	{
		if (load_config(args.config))
		{
			ret = -1;
			goto done;
		}
	}

	if (!args.server)
	{
		if (args.extra_count >= 1)
		{
			args.server = strdup(args.extras[0]);
		}
		else
		{
			fprintf(stderr, "Error! No server specified\n");
			ret = -1;
			goto done;
		}
	}

	draw_line();
	printf("Agent: %s\n", args.agentname);
	printf("SSL: %s\n", args.ssl ? "ON" : "OFF");
	printf("Server: %s:%d\n", args.server, args.port);
	printf("Test range: ");
	if (args.all)
	{
		printf("All\n");
	}
	else
	{
		print_range(args.range_count, args.range);
	}
	printf("Skip range: ");
	print_range(args.skiprange_count, args.skiprange);

	draw_line();
	printf("\n");

	// Perform the specified command.
	if (args.reports)
	{
		ret = update_reports();
	}
	else
	{
		run_cases();
	}

	draw_line();
	if (ret)
	{
		fprintf(stderr, "Failure!\n");
	}

	if (global_return)
	{
		libws_test_FAILURE("One or more tests failed!");
	}
	else
	{
		libws_test_SUCCESS("All tests ran OK!");
	}
	draw_line();

done:
	if (args.skip)
	{
		free(args.skip);
	}

	if (args.tests)
	{
		free(args.tests);
	}

	if (args.server)
	{
		free(args.server);
	}

	#if 0
	// TODO: Fix this.
	if (args.agentname)
	{
		free(args.agentname);
	}
	#endif

	cargo_destroy(&cargo);
	printf("Bye bye!\n");
	return ret | global_return;
}

