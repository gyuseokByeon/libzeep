// Copyright Maarten L. Hekkelman, Radboud University 2008-2013.
//        Copyright Maarten L. Hekkelman, 2014-2019
//  Distributed under the Boost Software License, Version 1.0.
//     (See accompanying file LICENSE_1_0.txt or copy at
//           http://www.boost.org/LICENSE_1_0.txt)

#include <string>

#include <zeep/xml/node.hpp>

namespace zeep
{
namespace xml
{

class document;

// --------------------------------------------------------------------
/// XPath's can contain variables. And variables can contain all kinds of data
/// like strings, numbers and even node_sets. If you want to use variables,
/// you can define a context, add your variables to it and then pass it on
/// in the xpath::evaluate method.

class context
{
public:
#ifndef LIBZEEP_DOXYGEN_INVOKED
	context();
	virtual ~context();
#endif

	template <typename T>
	void set(const std::string& name, const T& value);

	template <typename T>
	T get(const std::string& name);

#ifndef LIBZEEP_DOXYGEN_INVOKED
private:
	context(const context &);
	context& operator=(const context &);

	friend class xpath;

	struct context_imp* m_impl;
#endif
};

// --------------------------------------------------------------------
/// The actual xpath implementation. It expects an xpath in the constructor and
/// this path _must_ be UTF-8 encoded.

class xpath
{
public:
	xpath(const std::string& path);
	xpath(const char* path);
	xpath(const xpath& rhs);
	xpath& operator=(const xpath &);

#ifndef LIBZEEP_DOXYGEN_INVOKED
	virtual ~xpath();
#endif

	/// evaluate returns a node_set. If you're only interested in zeep::xml::element
	/// results, you should call the evaluate<element>() instantiation.
	template <typename NODE_TYPE>
	std::list<NODE_TYPE *> evaluate(const node& root) const;

	/// The second evaluate method is used for xpaths that contain variables.
	template <typename NODE_TYPE>
	std::list<NODE_TYPE *> evaluate(const node& root, context& ctxt) const;

	/// Returns true if the \a n node matches the XPath
	bool matches(const node* n) const;

#ifndef LIBZEEP_DOXYGEN_INVOKED
private:
	struct xpath_imp* m_impl;
#endif
};

#ifndef LIBZEEP_DOXYGEN_INVOKED

// --------------------------------------------------------------------
// template specialisations.

template <typename T>
void context::set(const std::string& name, const T& value)
{
	// only implemented for the cases specialised below
	assert(false);
}

template <>
void context::set<double>(const std::string& name, const double& value);

template <>
void context::set<std::string>(const std::string& name, const std::string& value);

template <typename T>
T context::get(const std::string& name)
{
	// only implemented for the cases specialised below
	assert(false);
}

template <>
double context::get<double>(const std::string& name);

template <>
std::string context::get<std::string>(const std::string& name);

// --------------------------------------------------------------------

template <>
node_set xpath::evaluate<node>(const node& root) const;

template <>
element_set xpath::evaluate<element>(const node& root) const;

template <>
node_set xpath::evaluate<node>(const node& root, context& ctxt) const;

template <>
element_set xpath::evaluate<element>(const node& root, context& ctxt) const;

#endif

} // namespace xml
} // namespace zeep
