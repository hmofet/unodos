css_error node_name(void *pw, void *n, css_qname *qname)
{
	lwc_string *node = n;

	UNUSED(pw);

	qname->name = lwc_string_ref(node);

	return CSS_OK;
}

css_error node_classes(void *pw, void *n,
		lwc_string ***classes, uint32_t *n_classes)
{
	UNUSED(pw);
	UNUSED(n);
	*classes = NULL;
	*n_classes = 0;
	return CSS_OK;
}

css_error node_id(void *pw, void *n, lwc_string **id)
{
	UNUSED(pw);
	UNUSED(n);
	*id = NULL;
	return CSS_OK;
}

css_error named_ancestor_node(void *pw, void *n,
		const css_qname *qname,
		void **ancestor)
{
	UNUSED(pw);
	UNUSED(n);
	UNUSED(qname);
	*ancestor = NULL;
	return CSS_OK;
}

css_error named_parent_node(void *pw, void *n,
		const css_qname *qname,
		void **parent)
{
	UNUSED(pw);
	UNUSED(n);
	UNUSED(qname);
	*parent = NULL;
	return CSS_OK;
}

css_error named_generic_sibling_node(void *pw, void *n,
		const css_qname *qname,
		void **sibling)
{
	UNUSED(pw);
	UNUSED(n);
	UNUSED(qname);
	*sibling = NULL;
	return CSS_OK;
}

css_error named_sibling_node(void *pw, void *n,
		const css_qname *qname,
		void **sibling)
{
	UNUSED(pw);
	UNUSED(n);
	UNUSED(qname);
	*sibling = NULL;
	return CSS_OK;
}

css_error parent_node(void *pw, void *n, void **parent)
{
	UNUSED(pw);
	UNUSED(n);
	*parent = NULL;
	return CSS_OK;
}

css_error sibling_node(void *pw, void *n, void **sibling)
{
	UNUSED(pw);
	UNUSED(n);
	*sibling = NULL;
	return CSS_OK;
}

css_error node_has_name(void *pw, void *n,
		const css_qname *qname,
		bool *match)
{
	lwc_string *node = n;
	UNUSED(pw);
	assert(lwc_string_caseless_isequal(node, qname->name, match) ==
			lwc_error_ok);
	return CSS_OK;
}

css_error node_has_class(void *pw, void *n,
		lwc_string *name,
		bool *match)
{
	UNUSED(pw);
	UNUSED(n);
	UNUSED(name);
	*match = false;
	return CSS_OK;
}

css_error node_has_id(void *pw, void *n,
		lwc_string *name,
		bool *match)
{
	UNUSED(pw);
	UNUSED(n);
	UNUSED(name);
	*match = false;
	return CSS_OK;
}

css_error node_has_attribute(void *pw, void *n,
		const css_qname *qname,
		bool *match)
{
	UNUSED(pw);
	UNUSED(n);
	UNUSED(qname);
	*match = false;
	return CSS_OK;
}

css_error node_has_attribute_equal(void *pw, void *n,
		const css_qname *qname,
		lwc_string *value,
		bool *match)
{
	UNUSED(pw);
	UNUSED(n);
	UNUSED(qname);
	UNUSED(value);
	*match = false;
	return CSS_OK;
}

css_error node_has_attribute_dashmatch(void *pw, void *n,
		const css_qname *qname,
		lwc_string *value,
		bool *match)
{
	UNUSED(pw);
	UNUSED(n);
	UNUSED(qname);
	UNUSED(value);
	*match = false;
	return CSS_OK;
}

css_error node_has_attribute_includes(void *pw, void *n,
		const css_qname *qname,
		lwc_string *value,
		bool *match)
{
	UNUSED(pw);
	UNUSED(n);
	UNUSED(qname);
	UNUSED(value);
	*match = false;
	return CSS_OK;
}

css_error node_has_attribute_prefix(void *pw, void *n,
		const css_qname *qname,
		lwc_string *value,
		bool *match)
{
	UNUSED(pw);
	UNUSED(n);
	UNUSED(qname);
	UNUSED(value);
	*match = false;
	return CSS_OK;
}

css_error node_has_attribute_suffix(void *pw, void *n,
		const css_qname *qname,
		lwc_string *value,
		bool *match)
{
	UNUSED(pw);
	UNUSED(n);
	UNUSED(qname);
	UNUSED(value);
	*match = false;
	return CSS_OK;
}

css_error node_has_attribute_substring(void *pw, void *n,
		const css_qname *qname,
		lwc_string *value,
		bool *match)
{
	UNUSED(pw);
	UNUSED(n);
	UNUSED(qname);
	UNUSED(value);
	*match = false;
	return CSS_OK;
}

css_error node_is_first_child(void *pw, void *n, bool *match)
{
	UNUSED(pw);
	UNUSED(n);
	*match = false;
	return CSS_OK;
}

css_error node_is_root(void *pw, void *n, bool *match)
{
	UNUSED(pw);
	UNUSED(n);
	*match = false;
	return CSS_OK;
}

css_error node_count_siblings(void *pw, void *n,
		bool same_name, bool after, int32_t *count)
{
	UNUSED(pw);
	UNUSED(n);
	UNUSED(same_name);
	UNUSED(after);
	*count = 1;
	return CSS_OK;
}

css_error node_is_empty(void *pw, void *n, bool *match)
{
	UNUSED(pw);
	UNUSED(n);
	*match = false;
	return CSS_OK;
}

css_error node_is_link(void *pw, void *n, bool *match)
{
	UNUSED(pw);
	UNUSED(n);
	*match = false;
	return CSS_OK;
}

css_error node_is_visited(void *pw, void *n, bool *match)
{
	UNUSED(pw);
	UNUSED(n);
	*match = false;
	return CSS_OK;
}

css_error node_is_hover(void *pw, void *n, bool *match)
{
	UNUSED(pw);
	UNUSED(n);
	*match = false;
	return CSS_OK;
}

css_error node_is_active(void *pw, void *n, bool *match)
{
	UNUSED(pw);
	UNUSED(n);
	*match = false;
	return CSS_OK;
}

css_error node_is_focus(void *pw, void *n, bool *match)
{
	UNUSED(pw);
	UNUSED(n);
	*match = false;
	return CSS_OK;
}

css_error node_is_enabled(void *pw, void *n, bool *match)
{
	UNUSED(pw);
	UNUSED(n);
	*match = false;
	return CSS_OK;
}

css_error node_is_disabled(void *pw, void *n, bool *match)
{
	UNUSED(pw);
	UNUSED(n);
	*match = false;
	return CSS_OK;
}

css_error node_is_checked(void *pw, void *n, bool *match)
{
	UNUSED(pw);
	UNUSED(n);
	*match = false;
	return CSS_OK;
}

css_error node_is_target(void *pw, void *n, bool *match)
{
	UNUSED(pw);
	UNUSED(n);
	*match = false;
	return CSS_OK;
}


css_error node_is_lang(void *pw, void *n,
		lwc_string *lang,
		bool *match)
{
	UNUSED(pw);
	UNUSED(n);
	UNUSED(lang);
	*match = false;
	return CSS_OK;
}

css_error node_presentational_hint(void *pw, void *node,
		uint32_t *nhints, css_hint **hints)
{
	UNUSED(pw);
	UNUSED(node);
	*nhints = 0;
	*hints = NULL;
	return CSS_OK;
}

css_error ua_default_for_property(void *pw, uint32_t property, css_hint *hint)
{
	UNUSED(pw);

	if (property == CSS_PROP_COLOR) {
		hint->data.color = 0x00000000;
		hint->status = CSS_COLOR_COLOR;
	} else if (property == CSS_PROP_FONT_FAMILY) {
		hint->data.strings = NULL;
		hint->status = CSS_FONT_FAMILY_SANS_SERIF;
	} else if (property == CSS_PROP_QUOTES) {
		/* Not exactly useful :) */
		hint->data.strings = NULL;
		hint->status = CSS_QUOTES_NONE;
	} else if (property == CSS_PROP_VOICE_FAMILY) {
		/** \todo Fix this when we have voice-family done */
		hint->data.strings = NULL;
		hint->status = 0;
	} else {
		return CSS_INVALID;
	}

	return CSS_OK;
}

static css_error set_libcss_node_data(void *pw, void *n,
		void *libcss_node_data)
{
	UNUSED(pw);
	UNUSED(n);

	/* Since we're not storing it, ensure node data gets deleted */
	css_libcss_node_data_handler(&select_handler, CSS_NODE_DELETED,
			pw, n, NULL, libcss_node_data);

	return CSS_OK;
}

static css_error get_libcss_node_data(void *pw, void *n,
		void **libcss_node_data)
{
	UNUSED(pw);
	UNUSED(n);
	*libcss_node_data = NULL;

	return CSS_OK;
}


