/* Function declarations. */
static css_error resolve_url(void *pw,
		const char *base, lwc_string *rel, lwc_string **abs);
static void die(const char *text, css_error code);

static css_error node_name(void *pw, void *node,
		css_qname *qname);
static css_error node_classes(void *pw, void *node,
		lwc_string ***classes, uint32_t *n_classes);
static css_error node_id(void *pw, void *node,
		lwc_string **id);
static css_error named_ancestor_node(void *pw, void *node,
		const css_qname *qname,
		void **ancestor);
static css_error named_parent_node(void *pw, void *node,
		const css_qname *qname,
		void **parent);
static css_error named_sibling_node(void *pw, void *node,
		const css_qname *qname,
		void **sibling);
static css_error named_generic_sibling_node(void *pw, void *node,
		const css_qname *qname,
		void **sibling);
static css_error parent_node(void *pw, void *node, void **parent);
static css_error sibling_node(void *pw, void *node, void **sibling);
static css_error node_has_name(void *pw, void *node,
		const css_qname *qname,
		bool *match);
static css_error node_has_class(void *pw, void *node,
		lwc_string *name,
		bool *match);
static css_error node_has_id(void *pw, void *node,
		lwc_string *name,
		bool *match);
static css_error node_has_attribute(void *pw, void *node,
		const css_qname *qname,
		bool *match);
static css_error node_has_attribute_equal(void *pw, void *node,
		const css_qname *qname,
		lwc_string *value,
		bool *match);
static css_error node_has_attribute_dashmatch(void *pw, void *node,
		const css_qname *qname,
		lwc_string *value,
		bool *match);
static css_error node_has_attribute_includes(void *pw, void *node,
		const css_qname *qname,
		lwc_string *value,
		bool *match);
static css_error node_has_attribute_prefix(void *pw, void *node,
		const css_qname *qname,
		lwc_string *value,
		bool *match);
static css_error node_has_attribute_suffix(void *pw, void *node,
		const css_qname *qname,
		lwc_string *value,
		bool *match);
static css_error node_has_attribute_substring(void *pw, void *node,
		const css_qname *qname,
		lwc_string *value,
		bool *match);
static css_error node_is_root(void *pw, void *node, bool *match);
static css_error node_count_siblings(void *pw, void *node,
		bool same_name, bool after, int32_t *count);
static css_error node_is_empty(void *pw, void *node, bool *match);
static css_error node_is_link(void *pw, void *node, bool *match);
static css_error node_is_visited(void *pw, void *node, bool *match);
static css_error node_is_hover(void *pw, void *node, bool *match);
static css_error node_is_active(void *pw, void *node, bool *match);
static css_error node_is_focus(void *pw, void *node, bool *match);
static css_error node_is_enabled(void *pw, void *node, bool *match);
static css_error node_is_disabled(void *pw, void *node, bool *match);
static css_error node_is_checked(void *pw, void *node, bool *match);
static css_error node_is_target(void *pw, void *node, bool *match);
static css_error node_is_lang(void *pw, void *node,
		lwc_string *lang, bool *match);
static css_error node_presentational_hint(void *pw, void *node,
		uint32_t *nhints, css_hint **hints);
static css_error ua_default_for_property(void *pw, uint32_t property,
		css_hint *hint);
static css_error set_libcss_node_data(void *pw, void *n,
		void *libcss_node_data);
static css_error get_libcss_node_data(void *pw, void *n,
		void **libcss_node_data);

