#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <link.h>

/* This should be defined before #include "utils.h" */
#define PR_FMT     "filter"
#define PR_DOMAIN  DBG_FILTER

#include "uftrace.h"
#include "utils/arch.h"
#include "utils/argspec.h"
#include "utils/filter.h"
#include "utils/utils.h"

static bool is_arm_machine(struct uftrace_filter_setting *setting)
{
	return setting->arch == UFT_CPU_ARM;
}

static int check_so_cb(struct dl_phdr_info *info, size_t size, void *data)
{
	const char *soname = data;
	int so_used = 0;

	if (!strncmp(basename(info->dlpi_name), soname, strlen(soname)))
		so_used = 1;

	return so_used;
}

/* check whether the given library name is in shared object list */
static int has_shared_object(const char *soname)
{
	static int so_used = -1;

	if (so_used != -1)
		return so_used;

	so_used = dl_iterate_phdr(check_so_cb, (void*)soname);

	return so_used;
}

/* argument_spec = arg1/i32,arg2/x64,... */
struct uftrace_arg_spec * parse_argspec(char *str,
					struct uftrace_filter_setting *setting)
{
	struct uftrace_arg_spec *arg;
	int fmt = ARG_FMT_AUTO;
	int size = setting->lp64 ? 8 : 4;
	int idx;
	int type;
	int bit;
	char *suffix;
	char *p;

	if (!strncmp(str, "arg", 3) && isdigit(str[3])) {
		idx = strtol(str + 3, &suffix, 0);
		type = ARG_TYPE_INDEX;
	}
	else if (!strncmp(str, "retval", 6)) {
		idx = RETVAL_IDX;
		type = ARG_TYPE_INDEX;
		suffix = str + 6;
	}
	else if (!strncmp(str, "fparg", 5 && isdigit(str[5]))) {
		idx = strtol(str+5, &suffix, 0);
		fmt = ARG_FMT_FLOAT;
		type = ARG_TYPE_FLOAT;
		size = sizeof(double);
	}
	else {
		pr_dbg("invalid argspec: %s\n", str);
		return NULL;
	}

	arg = xzalloc(sizeof(*arg));
	INIT_LIST_HEAD(&arg->list);

	if (suffix == NULL || *suffix == '\0')
		goto out;
	if (*suffix == '%')
		goto type;
	if (*suffix != '/')
		goto err;

	suffix++;
	switch (*suffix) {
	case 'd':
		fmt = ARG_FMT_AUTO;
		break;
	case 'i':
		fmt = ARG_FMT_SINT;
		break;
	case 'u':
		fmt = ARG_FMT_UINT;
		break;
	case 'x':
		fmt = ARG_FMT_HEX;
		break;
	case 's':
		fmt = ARG_FMT_STR;
		break;
	case 'c':
		fmt = ARG_FMT_CHAR;
		size = sizeof(char);
		break;
	case 'f':
		fmt = ARG_FMT_FLOAT;
		type = ARG_TYPE_FLOAT;
		size = sizeof(double);
		break;
	case 'S':
		if (has_shared_object("libc++.so")) {
			static bool warned = false;
			if (!warned) {
				pr_warn("std::string display for libc++.so is "
					"not supported.\n");
				warned = true;
			}
			goto err;
		}
		fmt = ARG_FMT_STD_STRING;
		break;
	case 'p':
		fmt = ARG_FMT_PTR;
		break;
	case 'e':
		fmt = ARG_FMT_ENUM;
		if (suffix[1] != ':' || (!isalpha(suffix[2]) && suffix[2] != '_')) {
			pr_use("invalid enum spec: %s\n", suffix);
			goto err;
		}
		arg->enum_str = xstrdup(&suffix[2]);

		p = strchr(arg->enum_str, '%');
		if (p)
			*p = '\0';
		pr_dbg2("parsing argspec for enum: %s\n", arg->enum_str);
		goto out;
	case 't':
		/* struct/union/class passed-by-value */
		fmt = ARG_FMT_STRUCT;
		size = strtol(&suffix[1], &suffix, 0);
		arg->struct_reg_cnt = 0;

		if (*suffix == ':') {
			suffix++;
			/* it can pass some fields in registers */
			while (*suffix == 'i' || *suffix == 'f') {
				arg->reg_types[arg->struct_reg_cnt++] = *suffix++;
				if (arg->struct_reg_cnt == sizeof(arg->reg_types))
					break;
			}
		}
		if (*suffix != '\0') {
			pr_use("invalid struct spec: %s\n", str);
			goto err;
		}
		goto out;
	default:
		if (fmt == ARG_FMT_FLOAT && isdigit(*suffix))
			goto size;
		pr_use("unsupported argument type: %s\n", str);
		goto err;
	}

	suffix++;
	if (*suffix == '\0')
		goto out;
	if (*suffix == '%')
		goto type;

size:
	bit = strtol(suffix, &suffix, 10);
	switch (bit) {
	case 8:
	case 16:
	case 32:
	case 64:
		size = bit / 8;
		break;
	case 80:
		if (fmt == ARG_FMT_FLOAT) {
			size = bit / 8;
			break;
		}
		/* fall through */
	default:
		pr_use("unsupported argument size: %s\n", str);
		goto err;
	}

type:
	if (*suffix == '%') {
		suffix++;

		if (!strncmp(suffix, "stack", 5)) {
			arg->stack_ofs = strtol(suffix+5, NULL, 0);
			type = ARG_TYPE_STACK;
		}
		else {
			arg->reg_idx = arch_register_number(setting->arch, suffix);
			type = ARG_TYPE_REG;

			if (arg->reg_idx < 0) {
				pr_use("unknown register name: %s\n", str);
				goto err;
			}
		}
	}
	else if (*suffix != '\0')
		goto err;

out:
	/* it seems ARM falls back 'long double' to 'double' */
	if (fmt == ARG_FMT_FLOAT && size == 10 && is_arm_machine(setting))
		size = 8;

	arg->idx  = idx;
	arg->fmt  = fmt;
	arg->size = size;
	arg->type = type;

	return arg;

err:
	free(arg);
	return NULL;
}

static void arrange_struct_args(struct uftrace_arg_spec *arg,
				struct uftrace_arg_arranger *aa,
				struct uftrace_filter_setting *setting)
{
	int i;
	short reg;
	char reg_types[4];
	int orig_int_reg = aa->next_int_reg;
	int orig_fp_reg = aa->next_fp_reg;

	memcpy(reg_types, arg->reg_types, sizeof(reg_types));

	arg->stack_ofs = 0;
	arg->struct_regs = xcalloc(arg->struct_reg_cnt, sizeof(*arg->struct_regs));

	for (i = 0; i < arg->struct_reg_cnt; i++) {
		if (reg_types[i] == 'i')
			reg = arch_register_at(setting->arch, true,
					       aa->next_int_reg++);
		else
			reg = arch_register_at(setting->arch, false,
					       aa->next_fp_reg++);

		if (reg < 0) {
			pr_dbg("struct register allocation failure\n");
			arg->type = ARG_TYPE_STACK;
			arg->stack_ofs = aa->next_stack_ofs;
			aa->next_stack_ofs += DIV_ROUND_UP(arg->size, sizeof(long));

			free(arg->struct_regs);
			arg->struct_regs = NULL;
			arg->struct_reg_cnt = 0;

			/* restore original state */
			aa->next_int_reg = orig_int_reg;
			aa->next_fp_reg = orig_fp_reg;
			return;
		}

		arg->struct_regs[i] = reg;
	}

	/* TODO: pass remaining fields via stack (for ARM?) */
}

/*
 * Re-arrange arguments position which might be affected by struct passed
 * by-value.  They can be passed by registers (maybe partially).  We convert
 * arguments given by index to have specific registers or stack offset.
 * It assumes all arguments are specified in the @args
 */
int reallocate_argspec(struct list_head *args,
		       struct uftrace_filter_setting *setting)
{
	struct uftrace_arg_spec *arg;
	struct uftrace_arg_arranger aa;
	int reg;

	memset(&aa, 0, sizeof(aa));

	list_for_each_entry(arg, args, list) {
		/*
		 * We should honor if user specified arguments in register or
		 * stack, use it as is and update the allocation status.
		 */
		switch (arg->type) {
		case ARG_TYPE_REG:
			reg = arch_register_index(setting->arch, arg->reg_idx);

			if (arg->fmt == ARG_FMT_FLOAT)
				aa.next_fp_reg = reg + 1;
			else
				aa.next_int_reg = reg + 1;
			break;

		case ARG_TYPE_STACK:
			aa.next_stack_ofs  = arg->stack_ofs;
			aa.next_stack_ofs += DIV_ROUND_UP(arg->size, sizeof(long));
			break;

		case ARG_TYPE_INDEX:
			arg->type = ARG_TYPE_REG;

			if (arg->fmt == ARG_FMT_STRUCT) {
				arrange_struct_args(arg, &aa, setting);
				break;
			}
			arg->reg_idx = arch_register_at(setting->arch, true,
							aa.next_int_reg++);
			if (arg->reg_idx < 0) {
				/*
				 * it's ok to leave next_int_reg incremented
				 * since it's already full
				 */
				arg->type = ARG_TYPE_STACK;
				arg->stack_ofs = aa.next_stack_ofs;

				aa.next_stack_ofs += DIV_ROUND_UP(arg->size,
								  sizeof(long));
			}
			break;

		case ARG_TYPE_FLOAT:
			arg->type = ARG_TYPE_REG;
			arg->reg_idx = arch_register_at(setting->arch, false,
							aa.next_fp_reg++);
			if (arg->reg_idx < 0) {
				/*
				 * it's ok to leave next_fp_reg incremented
				 * since it's already full
				 */
				arg->type = ARG_TYPE_STACK;
				arg->stack_ofs = aa.next_stack_ofs;

				aa.next_stack_ofs += DIV_ROUND_UP(arg->size,
								  sizeof(long));
			}
			break;

		default:
			break;
		}
	}
	return 0;
}
