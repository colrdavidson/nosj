#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define NJ_MIN(a, b) ((a) < (b) ? (a) : (b))

typedef enum {
	NJ_OK,
	NJ_ERROR,
	NJ_MORE,
	NJ_EOF,
} NJ_Advance_Type;

typedef enum {
	NJ_FAIL,
	NJ_ARRAY,
	NJ_OBJECT,
	NJ_END,
	NJ_NUMBER,
	NJ_STRING,
	NJ_BOOL,
	NJ_NULL,
} NJ_Token_Type;

char *nj_tok_type_to_str(NJ_Token_Type type) {
	switch (type) {
		case NJ_FAIL:   return "fail";
		case NJ_ARRAY:  return "array";
		case NJ_OBJECT: return "object";
		case NJ_END:    return "end";
		case NJ_NUMBER: return "number";
		case NJ_STRING: return "string";
		case NJ_BOOL:   return "bool";
		case NJ_NULL:   return "null";
	}
}

char *nj_adv_type_to_str(NJ_Advance_Type type) {
	switch (type) {
		case NJ_OK:    return "ok";
		case NJ_ERROR: return "error";
		case NJ_MORE:  return "more";
		case NJ_EOF:   return "eof";
	}
}

typedef struct {
	NJ_Token_Type type;
	uint64_t start;
	uint64_t len;
} NJ_Value;

typedef struct {
	NJ_Value val;

	NJ_Advance_Type adv_type;
	char *err_msg;
} NJ_Return;

typedef struct {
	char *buffer;
	uint64_t buf_size;

	uint64_t pos;
	uint64_t buf_pos;

	uint64_t skim_pos;
	uint64_t last_skim_pos;

	uint64_t file_size;

	int64_t depth;

	uint64_t line_no;
	bool in_value;
} NJ_Reader;

NJ_Reader nj_init(uint64_t file_size, char *buffer, uint64_t buf_size) {
	return (NJ_Reader){
		.pos = 0,
		.buf_pos = 0,
		.skim_pos = 0,
		.last_skim_pos = 0,
		.file_size = file_size,

		.buffer = buffer,
		.buf_size = buf_size,

		.depth = 0,
		.line_no = 1,
	};
}

NJ_Return nj_eof(void) {
	return (NJ_Return){.adv_type = NJ_EOF};
}
NJ_Return nj_error(char *msg) {
	return (NJ_Return){.adv_type = NJ_ERROR, .err_msg = msg};
}
NJ_Return nj_more(void) {
	return (NJ_Return){.adv_type = NJ_MORE};
}
NJ_Return nj_ok(void) {
	return (NJ_Return){.adv_type = NJ_OK};
}
NJ_Return nj_value(NJ_Value v) {
	return (NJ_Return){.adv_type = NJ_OK, .val = v};
}

bool nj_is_space(char ch) {
	return (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t' || ch == ',');
}

bool nj_is_digit(char ch) {
	return (ch >= '0' && ch <= '9');
}

bool nj_is_number_extra(char ch) {
	return (ch == '.' || ch == 'e' || ch == 'E');
}

NJ_Advance_Type nj_check(NJ_Reader *r) {
	if (r->pos > r->file_size) {
		return NJ_EOF;
	}

	if (r->buf_pos >= r->buf_size) {
		return NJ_MORE;
	}

	return NJ_OK;
}

void nj_step(NJ_Reader *r) {
	r->pos += 1;
	r->buf_pos += 1;
}

NJ_Return nj_report(NJ_Advance_Type type) {
	if (type == NJ_EOF) {
		return nj_eof();
	} else if (type == NJ_MORE) {
		return nj_more();
	} else if (type == NJ_ERROR) {
		return nj_error("unlabled error");
	} else {
		return nj_ok();
	}
}

NJ_Return nj_read(NJ_Reader *r) {
	NJ_Advance_Type adv = {};
	bool in_value = r->in_value;

next_char:
	if ((adv = nj_check(r)) != NJ_OK) {
		return nj_report(adv);
	}

	char ch = r->buffer[r->buf_pos];
	if (nj_is_space(ch) || (in_value && ch == ':')) {
		if (ch == ',' && in_value) {
			in_value = false;
		}
		if (ch == '\n') {
			r->line_no += 1;
		}
		nj_step(r);
		goto next_char;
	}

	r->skim_pos = r->pos;
	NJ_Value v = {};
	if (nj_is_digit(ch) || ch == '-' && in_value) {
		v.type = NJ_NUMBER;
		uint64_t start_pos = r->pos;
		v.start = r->buf_pos;

		for (;;) {
			nj_step(r);
			if ((adv = nj_check(r)) != NJ_OK) {
				return nj_report(adv);
			}

			char ch = r->buffer[r->buf_pos];
			if (!(nj_is_digit(ch) || ch == '.' || ch == 'E' || ch == 'e' || ch == '-' || ch == '+')) {
				v.len = r->pos - start_pos;
				goto success;
			}
		}
	}

	if (ch == '"') {
		nj_step(r);
		if ((adv = nj_check(r)) != NJ_OK) {
			return nj_report(adv);
		}

		v.type = NJ_STRING;
		uint64_t start_pos = r->pos;
		v.start = r->buf_pos;
		for (;;) {
			char ch = r->buffer[r->buf_pos];
			char prev_ch = r->buffer[r->buf_pos-1];
			if (ch == '"' && prev_ch != '\\') {
				v.len = r->pos - start_pos;
				nj_step(r);

				if (!in_value) {
					in_value = true;
				}
				goto success;
			}

			nj_step(r);
			if ((adv = nj_check(r)) != NJ_OK) {
				return nj_report(adv);
			}
		}
	}

	if (ch == 'n' || ch == 't' || ch == 'f') {
		char *check_str;
		uint64_t check_len;
		char null_tok[] = "null";
		char true_tok[] = "true";
		char false_tok[] = "false";

		if (ch == 'n') {
			check_str = null_tok;
			check_len = sizeof(null_tok) - 1;

			v.type = NJ_NULL;
		} else if (ch == 't') {
			check_str = true_tok;
			check_len = sizeof(true_tok) - 1;

			v.type = NJ_BOOL;
			v.start = r->buf_pos;
			v.len = check_len;
		} else if (ch == 'f') {
			check_str = false_tok;
			check_len = sizeof(false_tok) - 1;

			v.type = NJ_BOOL;
			v.start = r->buf_pos;
			v.len = check_len;
		}

		for (int i = 0; i < check_len; i++) {
			char ch = r->buffer[r->buf_pos];
			if (ch != check_str[i]) {
				return nj_error("invalid token!\n");
			}

			nj_step(r);
			if ((adv = nj_check(r)) != NJ_OK) {
				return nj_report(adv);
			}
		}

		goto success;
	}

	if (ch == '{' || ch == '[') {
		v.type = (ch == '{') ? NJ_OBJECT : NJ_ARRAY;
		r->depth += 1;
		nj_step(r);
		goto success;
	}

	if (ch == '}' || ch == ']') {
		v.type = NJ_END;
		r->depth -= 1;
		if (r->depth < 0) {
			return nj_error("Unmatched } or ]");
		}
		nj_step(r);
		goto success;
	}

	if (ch == '\0') {
		return nj_eof();
	}

	printf("%c || %d || line: %llu || pos: %llu\n", ch, ch, r->line_no, r->pos);
	return nj_error("Unknown token");

success:
	r->skim_pos = r->pos;
	r->in_value = in_value;
	return nj_value(v);
}

int main(int argc, char **argv) {
	if (argc < 2) {
		printf("Expected nosj <file>\n");
		return 1;
	}

	int fd = open(argv[1], O_RDONLY);
	uint64_t file_size = lseek(fd, 0, SEEK_END);
	lseek(fd, 0, SEEK_SET);

	uint64_t buf_size = 10 * 1024 * 1024;
	char *buffer = (char *)calloc(1, buf_size);
	uint64_t rem_len = NJ_MIN(file_size, buf_size);
	read(fd, buffer, rem_len);
	printf("reading json of size: %llu\n", file_size);

	NJ_Reader r = nj_init(file_size, buffer, buf_size);
	NJ_Return ret;
	do {
		ret = nj_read(&r);

		if (ret.adv_type == NJ_ERROR) {
			printf("got ret: %s || %s\n", nj_adv_type_to_str(ret.adv_type), ret.err_msg);
			return 1;
		}

		// FEED ME MORE DATA
		if (ret.adv_type == NJ_MORE) {
			if (r.skim_pos == r.last_skim_pos) {
				printf("looping? line: %llu || buf: %.*s\n", r.line_no, (int)r.buf_size, r.buffer);
				return 1;
			}
			r.pos = r.skim_pos;
			r.last_skim_pos = r.skim_pos;
			r.buf_pos = 0;

			memset(r.buffer, 0, r.buf_size);
			uint64_t rem_len = NJ_MIN(file_size - r.pos, r.buf_size);
			pread(fd, r.buffer, rem_len, r.pos);
			ret = (NJ_Return){};
			continue;
		}

		if (ret.adv_type == NJ_OK) {
			if (ret.val.type == NJ_FAIL) {
				printf("Got invalid type!\n");
				return 1;
			}

/*
			printf("got val: %s | ", nj_tok_type_to_str(ret.val.type));
			if (ret.val.type == NJ_STRING || ret.val.type == NJ_NUMBER || ret.val.type == NJ_BOOL) {
				printf("%.*s", (int)ret.val.len, r.buffer + ret.val.start);
			} else if (ret.val.type == NJ_OBJECT) { printf("{");
			} else if (ret.val.type == NJ_ARRAY) { printf("[");
			} else if (ret.val.type == NJ_END) { printf("}/]");
			} else if (ret.val.type == NJ_NULL) { printf("null"); }
			printf("\n");
*/
		}

		if (ret.adv_type == NJ_EOF) {
			break;
		}
	} while (ret.adv_type == NJ_OK);

	printf("finished parsing!\n");
	return 0;
}
