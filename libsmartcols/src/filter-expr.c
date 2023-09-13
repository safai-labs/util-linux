#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "smartcolsP.h"

struct filter_expr {
	struct filter_node node;
	enum filter_etype type;

	struct filter_node *left;
	struct filter_node *right;
};

struct filter_node *filter_new_expr(
			struct libscols_filter *fltr __attribute__((__unused__)),
			enum filter_etype type,
			struct filter_node *left,
			struct filter_node *right)
{
	struct filter_expr *n = (struct filter_expr *) __filter_new_node(
					F_NODE_EXPR, sizeof(struct filter_expr));

	n->type = type;
	switch (type) {
	case F_EXPR_AND:
	case F_EXPR_OR:
	case F_EXPR_EQ:
	case F_EXPR_NE:
	case F_EXPR_LE:
	case F_EXPR_LT:
	case F_EXPR_GE:
	case F_EXPR_GT:
	case F_EXPR_REG:
	case F_EXPR_NREG:
		n->left = left;
		n->right = right;
		break;
	case F_EXPR_NEG:
		n->right = right;
		break;

	}
	return (struct filter_node *) n;
}

void filter_free_expr(struct filter_expr *n)
{
	filter_unref_node(n->left);
	filter_unref_node(n->right);
	free(n);
}

static const char *expr_type_as_string(struct filter_expr *n)
{
	switch (n->type) {
	case F_EXPR_AND:
		return "AND";
	case F_EXPR_OR:
		return "OR";
	case F_EXPR_EQ:
		return "EQ";
	case F_EXPR_NE:
		return "NE";
	case F_EXPR_LE:
		return "LE";
	case F_EXPR_LT:
		return "LT";
	case F_EXPR_GE:
		return "GE";
	case F_EXPR_GT:
		return "GT";
	case F_EXPR_REG:
		return "REG";
	case F_EXPR_NREG:
		return "NREG";
	case F_EXPR_NEG:
		return "NOT";
	}
	return "";
}

void filter_dump_expr(struct ul_jsonwrt *json, struct filter_expr *n)
{
	ul_jsonwrt_object_open(json, "expr");
	ul_jsonwrt_value_s(json, "type", expr_type_as_string(n));

	if (n->left)
		filter_dump_node(json, n->left);
	if (n->right)
		filter_dump_node(json, n->right);

	ul_jsonwrt_object_close(json);
}

static int cast_node(struct libscols_filter *fltr,
		     struct libscols_line *ln,
		     enum filter_data type,
		     struct filter_node *n,
		     struct filter_param **result)
{
	struct filter_node *pr;
	int status = 0, rc;
	bool x;

	switch (n->type) {
	case F_NODE_EXPR:
		/* convert expression to a boolean param */
		rc = filter_eval_expr(fltr, ln, (struct filter_expr *) n, &status);
		if (rc)
			return rc;
		x = status != 0 ? true : false;
		pr = filter_new_param(NULL, F_DATA_BOOLEAN, 0, (void *) &x);
		if (!pr)
			return -ENOMEM;
		rc = filter_cast_param(fltr, ln, type, (struct filter_param *) pr, result);
		filter_unref_node(pr);
		break;
	case F_NODE_PARAM:
		rc = filter_cast_param(fltr, ln, type, (struct filter_param *) n, result);
		break;
	default:
		return -EINVAL;
	}

	return rc;
}

static enum filter_data node_get_datatype(struct filter_node *n)
{
	switch (n->type) {
	case F_NODE_EXPR:
		return F_DATA_BOOLEAN;
	case F_NODE_PARAM:
		return ((struct filter_param *) n)->type;
	}
	return F_DATA_NONE;
}

static enum filter_data guess_expr_datatype(struct filter_expr *n)
{
	enum filter_data type;
	enum filter_data l = node_get_datatype(n->left),
			 r = node_get_datatype(n->right);

	if (l == r)
		type = l;
	else {
		bool l_holder, r_holder;

		/* for expression like "FOO > 5.5" preffer type defined by a real param
		 * rather than by holder (FOO) */
		l_holder = is_filter_holder_param(n->left);
		r_holder = is_filter_holder_param(n->right);

		if (l_holder && !r_holder)
			type = r;
		else if (r_holder && !l_holder)
			type = l;
		else
			type = l;
	}

	DBG(FPARAM, ul_debugobj(n, " expr datatype: %d", type));
	return type;
}

int filter_eval_expr(struct libscols_filter *fltr, struct libscols_line *ln,
			struct filter_expr *n, int *status)
{
	int rc = 0;
	struct filter_param *l = NULL, *r = NULL;
	enum filter_etype oper = n->type;
	enum filter_data type;

	/* logical operators */
	switch (oper) {
	case F_EXPR_AND:
		rc = filter_eval_node(fltr, ln, n->left, status);
		if (rc == 0 && *status)
			rc = filter_eval_node(fltr, ln, n->right, status);
		return rc;
	case F_EXPR_OR:
		rc = filter_eval_node(fltr, ln, n->left, status);
		if (rc == 0 && !*status)
			rc = filter_eval_node(fltr, ln, n->right, status);
		return rc;
	case F_EXPR_NEG:
		rc = filter_eval_node(fltr, ln, n->right, status);
		if (rc == 0)
			*status = !*status;
		return rc;
	default:
		break;
	}

	type = guess_expr_datatype(n);

	/* compare data */
	rc = cast_node(fltr, ln, type, n->left, &l);
	if (!rc)
		rc = cast_node(fltr, ln, type, n->right, &r);
	if (!rc)
		rc = filter_compare_params(fltr, ln, oper, l, r, status);

	filter_unref_node((struct filter_node *) l);
	filter_unref_node((struct filter_node *) r);
	return rc;
}
