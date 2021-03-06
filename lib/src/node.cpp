// Copyright Maarten L. Hekkelman, Radboud University 2008-2013.
//        Copyright Maarten L. Hekkelman, 2014-2019
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#include <iostream>
#include <boost/algorithm/string.hpp>
// #include <boost/algorithm/string/join.hpp>
#include <boost/range.hpp>
#include <vector>
#include <typeinfo>

#include <zeep/xml/node.hpp>
#include <zeep/xml/writer.hpp>
#include <zeep/xml/xpath.hpp>
#include <zeep/exception.hpp>

namespace ba = boost::algorithm;

namespace zeep { namespace xml {

const char kWhiteSpaceChar[] = " ";

// --------------------------------------------------------------------

node::node()
	: m_parent(nullptr)
	, m_next(nullptr)
	, m_prev(nullptr)
{
}

node::~node()
{
	// avoid deep recursion and stack overflows
	while (m_next != nullptr)
	{
		node* n = m_next;
		m_next = n->m_next;
		n->m_next = nullptr;
		delete n;
	}
}

root_node* node::root()
{
	root_node* result = nullptr;
	if (m_parent != nullptr)
		result = m_parent->root();
	return result;
}

const root_node* node::root() const
{
	const root_node* result = nullptr;
	if (m_parent != nullptr)
		result = m_parent->root();
	return result;
}

bool node::equals(const node* n) const
{
	bool result = typeid(this) == typeid(n);

	if (result)
	{
		if (m_next != nullptr and n->m_next != nullptr)
			result = m_next->equals(n->m_next);
		else
			result = (m_next == nullptr and n->m_next == nullptr);
	}
	
	return result;
}

node* node::clone() const
{
	assert(false);
	throw zeep::exception("cannot clone this");
	return nullptr;
}

std::string node::lang() const
{
	std::string result;
	if (m_parent != nullptr)
		result = m_parent->lang();
	return result;
}

void node::insert_sibling(node* n, node* before)
{
//#if DEBUG
//validate();
//n->validate();
//if (before) before->validate();
//#endif

	node* p = this;
	while (p->m_next != nullptr and p->m_next != before)
		p = p->m_next;

	if (p->m_next != before and before != nullptr)
		throw zeep::exception("before argument in insert_sibling is not valid");
	
	p->m_next = n;
	n->m_prev = p;
	n->m_parent = m_parent;
	n->m_next = before;
	
	if (before != nullptr)
		before->m_prev = n;

//#if DEBUG
//validate();
//n->validate();
//if (before) before->validate();
//#endif
}

void node::remove_sibling(node* n)
{
//#if DEBUG
//validate();
//n->validate();
//#endif

	assert (this != n);
	if (this == n)
		throw exception("inconsistent node tree");

	node* p = this;
	while (p != nullptr and p->m_next != n)
		p = p->m_next;

	if (p != nullptr and p->m_next == n)
	{
		p->m_next = n->m_next;
		if (p->m_next != nullptr)
			p->m_next->m_prev = p;
		n->m_next = n->m_prev = n->m_parent = nullptr;
	}
	else
		throw exception("remove for a node not found in the list");

//#if DEBUG
//validate();
//n->validate();
//#endif 
}

void node::parent(container* n)
{
	assert(m_parent == nullptr);
	m_parent = n;
}

std::string node::qname() const
{
	return "";
}

std::string node::name() const
{
	std::string qn = qname();
	std::string::size_type s = qn.find(':');
	if (s != std::string::npos)
		qn.erase(0, s + 1);
	return qn;
}

std::string node::prefix() const
{
	std::string qn = qname();
	std::string::size_type s = qn.find(':');

	std::string p;

	if (s != std::string::npos)
		p = qn.substr(0, s);

	return p;
}

std::string node::ns() const
{
	std::string result, p = prefix();
	result = namespace_for_prefix(p);
	return result;
}

std::string node::namespace_for_prefix(const std::string& prefix) const
{
	std::string result;
	if (m_parent != nullptr)
		result = m_parent->namespace_for_prefix(prefix);
	return result;
}

std::string node::prefix_for_namespace(const std::string& uri) const
{
	std::string result;
	if (m_parent != nullptr)
		result = m_parent->prefix_for_namespace(uri);
	return result;
}

void node::write_content(std::ostream& os, const char* sep) const
{
	// do nothing
}

void node::validate()
{
	if (m_parent and dynamic_cast<element*>(this) != nullptr and
			(find(m_parent->node_begin(), m_parent->node_end(), this) == m_parent->node_end()))
		throw exception("validation error: parent does not know node");
	if (m_next and m_next->m_prev != this)
		throw exception("validation error: m_next->m_prev != this");
	if (m_prev and m_prev->m_next != this)
		throw exception("validation error: m_prev->m_next != this");
	
	node* n = this;
	while (n != nullptr and n->m_next != this)
		n = n->m_next;
	if (n == this)
		throw exception("cycle in node list");

	n = this;
	while (n != nullptr and n->m_prev != this)
		n = n->m_prev;
	if (n == this)
		throw exception("cycle in node list");
	
	if (m_next)
		m_next->validate();
}

// --------------------------------------------------------------------
// container_node

container::container()
	: m_child(nullptr)
	, m_last(nullptr)
{
}

container::~container()
{
	delete m_child;
}

template<>
std::list<text*> container::children<text>() const
{
	std::list<text*> result;
	
	node* child = m_child;
	while (child != nullptr)
	{
		if (typeid(*child) == typeid(text))
			result.push_back(static_cast<text*>(child));
		child = child->next();
	}
	
	return result;
}

template<>
node_set container::children<node>() const
{
	node_set result;
	
	node* child = m_child;
	while (child != nullptr)
	{
		result.push_back(child);
		child = child->next();
	}
	
	return result;
}

template<>
element_set container::children<element>() const
{
	element_set result;
	
	node* child = m_child;
	while (child != nullptr)
	{
		if (typeid(*child) == typeid(element))
			result.push_back(static_cast<element*>(child));
		child = child->next();
	}
	
	return result;
}

void container::append(node_ptr n)
{
//#if DEBUG
//validate();
//n->validate();
//#endif

	if (n != nullptr)
	{
		if (n->m_parent != nullptr)
			throw exception("attempt to append node that has already a parent");
		if (n == nullptr)
			throw exception("attempt to append nullptr node");
		
		if (m_child == nullptr)
		{
			m_last = m_child = n;
			m_child->m_next = m_child->m_prev = nullptr;
			n->parent(this);
		}
		else
		{
			m_last->insert_sibling(n, nullptr);
			m_last = n;
		}
	}

//#if DEBUG
//validate();
//n->validate();
//#endif
}

void container::remove(node_ptr n)
{
//#if DEBUG
//validate();
//n->validate();
//#endif
	
	if (n != nullptr)
	{
		if (n->m_parent != this)
			throw exception("attempt to remove node whose parent is invalid");
		
		if (m_child == n)
		{
			m_child = m_child->m_next;
			if (m_child != nullptr)
				m_child->m_prev = nullptr;
			else
				m_last = nullptr;
			n->m_next = n->m_prev = n->m_parent = nullptr;
		}
		else
		{
			if (m_last == n)
				m_last = n->m_prev;
	
			m_child->remove_sibling(n);
		}
	}
	
//#if DEBUG
//validate();
//n->validate();
//#endif
}

//element_set container::find(const xpath& path) const
//{
//	return path.evaluate<element>(*this);
//}
//
//element* container::find_first(const xpath& path) const
//{
//	element_set s = path.evaluate<element>(*this);
//	
//	element* result = nullptr;
//	if (not s.empty())
//		result = s.front();
//	return result;
//}

element_set container::find(const char* path) const
{
	return xpath(path).evaluate<element>(*this);
}

element* container::find_first(const char* path) const
{
	element_set s = xpath(path).evaluate<element>(*this);
	
	element* result = nullptr;
	if (not s.empty())
		result = s.front();
	return result;
}

void container::find(const char* path, node_set& nodes) const
{
	nodes = xpath(path).evaluate<node>(*this);
}

void container::find(const char* path, element_set& elements) const
{
	elements = xpath(path).evaluate<element>(*this);
}

node* container::find_first_node(const char* path) const
{
	node_set s = xpath(path).evaluate<node>(*this);
	
	node* result = nullptr;
	if (not s.empty())
		result = s.front();
	return result;
}

container::size_type container::size() const
{
	size_type result = 0;
	for (node* n = m_child; n != m_last; n = n->m_next)
		++result;
	return result;
}

bool container::empty() const
{
	return m_child == nullptr;
}

node* container::front() const
{
	return m_child;
}

node* container::back() const
{
	return m_last;
}

void container::swap(container& cnt)
{
	std::swap(m_child, cnt.m_child);
	std::swap(m_last, cnt.m_last);
	for (node* n = m_child; n != m_last; n = n->m_next)
		n->m_parent = this;
	for (node* n = cnt.m_child; n != cnt.m_last; n = n->m_next)
		n->m_parent = &cnt;
}

void container::clear()
{
	delete m_child;
	m_child = m_last = nullptr;
}

void container::push_front(node* n)
{
//#if DEBUG
//validate();
//n->validate();
//#endif

	if (n == nullptr)
		throw exception("attempt to insert nullptr node");
	if (n->m_next != nullptr or n->m_prev != nullptr)
		throw exception("attempt to insert a node that has next or prev");
	if (n->m_parent != nullptr)
		throw exception("attempt to insert node that already has a parent");

	n->parent(this);
	n->m_next = m_child;
	if (m_child != nullptr)
		m_child->m_prev = n;

	m_child = n;
	if (m_last == nullptr)
		m_last = m_child;

//#if DEBUG
//validate();
//n->validate();
//#endif
}

void container::pop_front()
{
//#if DEBUG
//validate();
//#endif

	if (m_child != nullptr)
	{
		node* n = m_child;

		m_child = m_child->m_next;
		if (m_child != nullptr)
			m_child->m_prev = nullptr;
		
		if (n == m_last)
			m_last = nullptr;
		
		n->m_next = nullptr;
		delete n;
	}

//#if DEBUG
//validate();
//#endif
}

void container::push_back(node* n)
{
//#if DEBUG
//validate();
//n->validate();
//#endif

	if (n == nullptr)
		throw exception("attempt to insert nullptr node");
	
	if (n->m_next != nullptr or n->m_prev != nullptr)
		throw exception("attempt to insert a node that has next or prev");
	if (n->m_parent != nullptr)
		throw exception("attempt to insert node that already has a parent");

	if (m_child == nullptr)
	{
		m_last = m_child = n;
		m_child->m_next = m_child->m_prev = nullptr;
		n->parent(this);
	}
	else
	{
		m_last->insert_sibling(n, nullptr);
		m_last = n;
	}

//#if DEBUG
//validate();
//n->validate();
//#endif
}

void container::pop_back()
{
//#if DEBUG
//validate();
//#endif

	if (m_last != nullptr)
	{
		if (m_last == m_child)
		{
			delete m_child;
			m_child = m_last = nullptr;
		}
		else
		{
			node* n = m_last;
			m_last = m_last->m_prev;
			m_last->m_next = nullptr;
			
			n->m_prev = nullptr;
			delete n;
		}
	}

//#if DEBUG
//validate();
//#endif
}

container::node_iterator container::insert(node* position, node* n)
{
	if (n == nullptr)
		throw exception("attempt to insert nullptr node");
	if (position and position->m_parent != this)
		throw exception("position has another parent");
	if (n->m_next != nullptr or n->m_prev != nullptr)
		throw exception("attempt to insert a node that has next or prev");
	if (n->m_parent != nullptr)
		throw exception("attempt to insert node that already has a parent");

//#if DEBUG
//validate();
//n->validate();
//	position->validate();
//#endif
	
	if (m_child == nullptr)
	{
		if (position != nullptr) throw exception("invalid position for empty container");
		m_child = n;
		m_child->m_next = m_child->m_prev = nullptr;
		n->parent(this);
	}
	else if (m_child == position)	// n becomes the first in the list
	{
		n->parent(this);
		n->m_next = m_child;
		m_child->m_prev = n;
		m_child = n;
		m_child->m_prev = nullptr;
	}
	else
		m_child->insert_sibling(n, position);

	if (m_last == nullptr)
		m_last = m_child;

//#if DEBUG
//validate();
//n->validate();
//#endif

	return node_iterator(n);
}

void container::validate()
{
//#if DEBUG
//	node::validate();
//#endif
	
	if (m_child or m_last)
	{
		if (m_child == nullptr or m_last == nullptr)
			throw exception("m_child/m_last error");
		if (std::find(node_begin(), node_end(), m_child) == node_end())
			throw exception("cannot find m_child in this");
		if (std::find(node_begin(), node_end(), m_last) == node_end())
			throw exception("cannot find m_last in this");
		if (m_child->m_prev != nullptr)
			throw exception("m_child is not first in list");
		if (m_last->m_next != nullptr)
			throw exception("m_last is not last in list");

//#if DEBUG
//		m_child->validate();
//#endif
	}
}

// --------------------------------------------------------------------
// root_node

root_node::root_node()
{
}

root_node::~root_node()
{
}

root_node* root_node::root()
{
	return this;
}

const root_node* root_node::root() const
{
	return this;
}

std::string root_node::str() const
{
	std::string result;
	element* e = child_element();
	if (e != nullptr)
		result = e->str();
	return result;
}

element* root_node::child_element() const
{
	element* result = nullptr;

	node* n = m_child;
	while (n != nullptr and result == nullptr)
	{
		result = dynamic_cast<element*>(n);
		n = n->next();
	}
	
	return result;
}

void root_node::child_element(element* child)
{
	element* e = child_element();
	if (e != nullptr)
	{
		container::remove(e);
		delete e;
	}
	
	if (child != nullptr)
		container::append(child);
}

void root_node::write(writer& w) const
{
	node* child = m_child;
	while (child != nullptr)
	{
		child->write(w);
		child = child->next();
	}
}

bool root_node::equals(const node* n) const
{
	bool result = false;
	if (typeid(*n) == typeid(*this))
	{
		result = true;
		
		const root_node* e = static_cast<const root_node*>(n);
		
		if (m_child != nullptr and e->m_child != nullptr)
			result = m_child->equals(e->m_child);
		else
			result = m_child == nullptr and e->m_child == nullptr;
	}

	return result;
}

void root_node::append(node* n)
{
	if (dynamic_cast<element*>(n) != nullptr and child_element() == nullptr)
		child_element(static_cast<element*>(n));
	else if (dynamic_cast<processing_instruction*>(n) == nullptr and
		dynamic_cast<comment*>(n) == nullptr)
	{
		throw exception("can only append comment and processing instruction nodes to a root_node");
	}
	else
		container::append(n);
}

// --------------------------------------------------------------------
// comment

void comment::write(writer& w) const
{
	w.comment(m_text);
}

bool comment::equals(const node* n) const
{
	return
		node::equals(n) and
		dynamic_cast<const comment*>(n) != nullptr and
		m_text == static_cast<const comment*>(n)->m_text;
}

node* comment::clone() const
{
	return new comment(m_text);
}

// --------------------------------------------------------------------
// processing_instruction

void processing_instruction::write(writer& w) const
{
	w.processing_instruction(m_target, m_text);
}

bool processing_instruction::equals(const node* n) const
{
	return
		node::equals(n) and
		dynamic_cast<const processing_instruction*>(n) != nullptr and
		m_text == static_cast<const processing_instruction*>(n)->m_text;
}

node* processing_instruction::clone() const
{
	return new processing_instruction(m_target, m_target);
}

// --------------------------------------------------------------------
// text

void text::write(writer& w) const
{
	w.content(m_text);
}

bool text::equals(const node* n) const
{
	bool result = false;
	auto t = dynamic_cast<const text*>(n);

	if (node::equals(n) and t != nullptr)
	{
		std::string text = m_text;
		ba::trim(text);

		std::string ttext = t->m_text;
		ba::trim(ttext);

		result = text == ttext;
	}

	return result;
}

node* text::clone() const
{
	return new text(m_text);
}

// --------------------------------------------------------------------
// cdata

void cdata::write(writer& w) const
{
	w.cdata(m_text);
}

bool cdata::equals(const node* n) const
{
	return
		node::equals(n) and
		dynamic_cast<const cdata*>(n) != nullptr and
		m_text == static_cast<const cdata*>(n)->m_text;
}

node* cdata::clone() const
{
	return new cdata(m_text);
}

// --------------------------------------------------------------------
// attribute

void attribute::write(writer& w) const
{
	assert(false);
}

bool attribute::equals(const node* n) const
{
	bool result = false;
	if (node::equals(n))
	{
		const attribute* a = static_cast<const attribute*>(n);
		
		result = m_qname == a->m_qname and
				 m_value == a->m_value;
	}
	return result;
}

node* attribute::clone() const
{
	return new attribute(m_qname, m_value, m_id);
}

// --------------------------------------------------------------------
// name_space

void name_space::write(writer& w) const
{
	assert(false);
}

bool name_space::equals(const node* n) const
{
	bool result = false;
	if (node::equals(n))
	{
		const name_space* ns = static_cast<const name_space*>(n);
		result = m_prefix == ns->m_prefix and m_uri == ns->m_uri;
	}
	return result;
}

node* name_space::clone() const
{
	return new name_space(m_prefix, m_uri);
}

// --------------------------------------------------------------------
// element

element::element(const std::string& qname)
	: m_qname(qname)
	, m_attribute(nullptr)
	, m_name_space(nullptr)
{
}

element::~element()
{
	delete m_attribute;
	delete m_name_space;
}

std::string element::str() const
{
	std::string result;
	
	const node* child = m_child;
	while (child != nullptr)
	{
		result += child->str();
		child = child->next();
	}
	
	return result;
}

std::string element::content() const
{
	std::string result;
	
	const node* child = m_child;
	while (child != nullptr)
	{
		if (dynamic_cast<const text*>(child) != nullptr)
			result += child->str();
		child = child->next();
	}
	
	return result;
}

void element::content(const std::string& s)
{
	node* child = m_child;
	
	// remove all existing text nodes (including cdata ones)
	while (child != nullptr)
	{
		node* next = child->next();
		
		if (dynamic_cast<text*>(child) != nullptr)
		{
			container::remove(child);
			delete child;
		}

		child = next;
	}

	// and add a new text node with the content
	append(new text(s));
}

void element::add_text(const std::string& s)
{
	text* textNode = dynamic_cast<text*>(m_last);
	
	if (textNode != nullptr and dynamic_cast<cdata*>(textNode) == nullptr)
		textNode->append(s);
	else
		append(new text(s));
}

void element::set_text(const std::string& s)
{
	clear();
	add_text(s);
}

attribute_set element::attributes() const
{
	attribute_set result;
	
	node* attr = m_attribute;
	while (attr != nullptr)
	{
		result.push_back(static_cast<attribute*>(attr));
		attr = attr->next();
	}
	
	return result;
}

name_space_list element::name_spaces() const
{
	name_space_list result;
	
	node* ns = m_name_space;
	while (ns != nullptr)
	{
		result.push_back(static_cast<name_space*>(ns));
		ns = ns->next();
	}
	
	return result;
}

std::string element::get_attribute(const std::string& qname) const
{
	std::string result;

	for (attribute* attr = m_attribute; attr != nullptr; attr = static_cast<attribute*>(attr->next()))
	{
		if (attr->qname() == qname)
		{
			result = attr->value();
			break;
		}
	}

	return result;
}

attribute* element::get_attribute_node(const std::string& qname) const
{
	attribute* attr = m_attribute;

	while (attr != nullptr)
	{
		if (attr->qname() == qname)
			break;
		attr = static_cast<attribute*>(attr->next());
	}

	return attr;
}

void element::set_attribute(const std::string& qname, const std::string& value, bool id)
{
	attribute* attr = get_attribute_node(qname);
	if (attr != nullptr)
		attr->value(value);
	else
	{
		attr = new attribute(qname, value, id);
		
		if (m_attribute == nullptr)
		{
			m_attribute = attr;
			m_attribute->parent(this);
		}
		else
			m_attribute->insert_sibling(attr, nullptr);
	}
}

void element::remove_attribute(const std::string& qname)
{
	attribute* n = get_attribute_node(qname);
	if (n != nullptr)
		remove_attribute(n);
}

void element::remove_attribute(attribute* attr)
{
	assert(attr->m_parent == this);
	
	if (m_attribute == attr)
	{
		m_attribute = static_cast<attribute*>(m_attribute->m_next);
		if (m_attribute != nullptr)
			m_attribute->m_prev = nullptr;
	}
	else
		m_attribute->remove_sibling(attr);
}

std::string element::namespace_for_prefix(const std::string& prefix) const
{
	std::string result;
	
	for (name_space* ns = m_name_space; ns != nullptr; ns = static_cast<name_space*>(ns->next()))
	{
		if (ns->prefix() == prefix)
		{
			result = ns->uri();
			break;
		}
	}
	
	if (result.empty() and dynamic_cast<element*>(m_parent) != nullptr)
		result = static_cast<element*>(m_parent)->namespace_for_prefix(prefix);
	
	return result;
}

std::string element::prefix_for_namespace(const std::string& uri) const
{
	std::string result;
	
	for (name_space* ns = m_name_space; ns != nullptr; ns = static_cast<name_space*>(ns->next()))
	{
		if (ns->uri() == uri)
		{
			result = ns->prefix();
			break;
		}
	}
	
	if (result.empty() and dynamic_cast<element*>(m_parent) != nullptr)
		result = static_cast<element*>(m_parent)->prefix_for_namespace(uri);
	
	return result;
}

void element::set_name_space(const std::string& prefix, const std::string& uri)
{
	name_space* ns;
	for (ns = m_name_space; ns != nullptr; ns = static_cast<name_space*>(ns->next()))
	{
		if (ns->prefix() == prefix)
		{
			ns->uri(uri);
			break;
		}
	}
	
	if (ns == nullptr)
		add_name_space(new name_space(prefix, uri));
}

void element::add_name_space(name_space* ns)
{
	if (m_name_space == nullptr)
	{
		m_name_space = ns;
		m_name_space->parent(this);
	}
	else
		m_name_space->insert_sibling(ns, nullptr);
}

std::string element::lang() const
{
	std::string result = get_attribute("xml:lang");
	if (result.empty())
		result = node::lang();
	return result;
}

std::string element::id() const
{
	std::string result;
	
	attribute* attr = m_attribute;
	while (attr != nullptr)
	{
		if (attr->id())
		{
			result = attr->value();
			break;
		}
		attr = static_cast<attribute*>(attr->next());
	}
	
	return result;
}

void element::write_content(std::ostream& os, const char* sep) const
{
	node* child = m_child;
	while (child != nullptr)
	{
		child->write_content(os, sep);
		child = child->next();
		
		if (sep != nullptr)
			os << sep;
	}
}

void element::write(writer& w) const
{
	w.start_element(m_qname);

	attribute* attr = m_attribute;
	while (attr != nullptr)
	{
		w.attribute(attr->qname(), attr->value());
		attr = static_cast<attribute*>(attr->next());
	}
	
	name_space* ns = m_name_space;
	while (ns != nullptr)
	{
		if (ns->prefix().empty())
			w.attribute("xmlns", ns->uri());
		else
			w.attribute(std::string("xmlns:") + ns->prefix(), ns->uri());
		ns = static_cast<name_space*>(ns->next());
	}

	node* child = m_child;
	while (child != nullptr)
	{
		child->write(w);
		child = child->next();
	}

	w.end_element();
}

bool element::equals(const node* n) const
{
	bool result = false;
	if (node::equals(n) and
		m_qname == static_cast<const element*>(n)->m_qname)
	{
		result = true;
		
		const element* e = static_cast<const element*>(n);
		
		if (m_child != nullptr and e->m_child != nullptr)
			result = m_child->equals(e->m_child);
		else
			result = m_child == nullptr and e->m_child == nullptr;
		
		if (result and m_attribute != nullptr and e->m_attribute != nullptr)
			result = m_attribute->equals(e->m_attribute);
		else
			result = m_attribute == nullptr and e->m_attribute == nullptr;

		if (result and m_name_space != nullptr and e->m_name_space != nullptr)
			result = m_name_space->equals(e->m_name_space);
		else
			result = m_name_space == nullptr and e->m_name_space == nullptr;

	}

	return result;
}

node* element::clone() const
{
	element* result = new element(m_qname);
	
	attribute* attr = m_attribute;
	while (attr != nullptr)
	{
		result->set_attribute(attr->qname(), attr->value(), attr->id());
		attr = static_cast<attribute*>(attr->next());
	}
	
	name_space* ns = m_name_space;
	while (ns != nullptr)
	{
		result->add_name_space(static_cast<name_space*>(ns->clone()));
		ns = static_cast<name_space*>(ns->next());
	}

	node* child = m_child;
	while (child != nullptr)
	{
		result->push_back(child->clone());
		child = child->next();
	}
	
	return result;
}

// --------------------------------------------------------------------
// operator<<

std::ostream& operator<<(std::ostream& lhs, const node& rhs)
{
	if (typeid(rhs) == typeid(node))
		std::cout << "base class???";
	else if (typeid(rhs) == typeid(root_node))
		std::cout << "root_node";
	else if (typeid(rhs) == typeid(element))
	{
		std::cout << "element <" << static_cast<const element&>(rhs).qname();
		
		const element* e = static_cast<const element*>(&rhs);
		for (const attribute* attr: e->attributes())
			std::cout << ' ' << attr->qname() << "=\"" << attr->value() << '"';
		
		std::cout << '>';
	}
	else if (typeid(rhs) == typeid(comment))
		std::cout << "comment";
	else if (typeid(rhs) == typeid(processing_instruction))
		std::cout << "processing_instruction";
	else
		std::cout << typeid(rhs).name();
		
	return lhs;
}

} // xml
} // zeep
